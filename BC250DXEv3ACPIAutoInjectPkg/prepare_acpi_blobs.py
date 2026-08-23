#!/usr/bin/env python3
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
TABLE_ROOT = ROOT / 'AcpiTables'
OUT_HEADER = ROOT / 'BC250DXEv3ACPIAutoInjectDxe' / 'AcpiTableBlobs.h'
BUILD_ROOT = ROOT / 'Build' / 'AcpiTables'

GENERATED_ROOT = TABLE_ROOT / 'Generated'


def write_generated_sources() -> list[tuple[str, str, Path]]:
    """Create one common table and one table per compacted SMT pair."""
    GENERATED_ROOT.mkdir(parents=True, exist_ok=True)
    tables = []

    def add(symbol: str, name: str, filename: str, text: str) -> None:
        source = GENERATED_ROOT / filename
        source.write_text(text)
        tables.append((symbol, name, source))

    add('PstateCommon', 'PSTATE-COMMON', 'SSDT-PSTATE-COMMON.dsl', '''DefinitionBlock ("", "SSDT", 2, "BC25", "PSTCOM", 1)\n{\n    Name (PPCT, Package () {\n        ResourceTemplate () { Register (FFixedHW, 64, 0, 0x00000000C0010062) },\n        ResourceTemplate () { Register (FFixedHW, 64, 0, 0x0000000000000000) }\n    })\n    Name (PPSS, Package () {\n        Package () { 3200, 0, 1000, 1000, 0, 0 }, Package () { 2550, 0, 1000, 1000, 1, 1 },\n        Package () { 2325, 0, 1000, 1000, 2, 2 }, Package () { 1960, 0, 1000, 1000, 3, 3 },\n        Package () { 1820, 0, 1000, 1000, 4, 4 }, Package () { 1600, 0, 1000, 1000, 5, 5 },\n        Package () { 1271, 0, 1000, 1000, 6, 6 }, Package () { 800, 0, 1000, 1000, 7, 7 }\n    })\n    Name (PPSD, Package () { Package () { 0x05, 0x00, 0x00000001, 0x000000FE, 0x00000002 } })\n}\n''')

    add('PartialCstateCommon', 'CSTATE-COMMON-2', 'SSDT-CSTATE-COMMON-2.dsl', '''DefinitionBlock ("", "SSDT", 2, "BC25", "CSTCOM2", 1)\n{\n    Method (PCST, 0, NotSerialized) { Return (Package () { 0x02,\n        Package () { ResourceTemplate () { Register (FFixedHW, 0x02, 0x02, 0) }, 0x01, 0x0001, 0 },\n        Package () { ResourceTemplate () { Register (SystemIO, 0x08, 0, 0x414, 1) }, 0x02, 0x0190, 0 }\n    }) }\n}\n''')
    add('FullCstateCommon', 'CSTATE-COMMON-3', 'SSDT-CSTATE-COMMON-3.dsl', '''DefinitionBlock ("", "SSDT", 2, "BC25", "CSTCOM3", 2)\n{\n    Method (PCST, 0, NotSerialized) { Return (Package () { 0x03,\n        Package () { ResourceTemplate () { Register (FFixedHW, 0x02, 0x02, 0) }, 0x01, 0x0001, 0 },\n        Package () { ResourceTemplate () { Register (SystemIO, 0x08, 0, 0x414, 1) }, 0x02, 0x015E, 0 },\n        Package () { ResourceTemplate () { Register (SystemIO, 0x08, 0, 0x415, 1) }, 0x03, 0x0190, 0 }\n    }) }\n}\n''')

    for pair in range(8):
        a, b = pair * 2, pair * 2 + 1
        p = f'{a:03X}'
        q = f'{b:03X}'
        ext = f'    External (\\_PR.P{p}, ProcessorObj)\n    External (\\_PR.P{q}, ProcessorObj)\n    External (\\PPCT, UnknownObj)\n    External (\\PPSS, UnknownObj)\n    External (\\PPSD, UnknownObj)'
        cstate_ext = ext + '\n    External (\\PCST, MethodObj)'
        methods = f'''DefinitionBlock ("", "SSDT", 2, "BC25", "PAIR{pair}", 1)\n{{\n{ext}\n    Scope (\\_PR.P{p}) {{ Method (_PCT, 0, NotSerialized) {{ Return (\\PPCT) }} Method (_PSS, 0, NotSerialized) {{ Return (\\PPSS) }} Method (_PSD, 0, NotSerialized) {{ Return (\\PPSD) }} }}\n    Scope (\\_PR.P{q}) {{ Method (_PCT, 0, NotSerialized) {{ Return (\\PPCT) }} Method (_PSS, 0, NotSerialized) {{ Return (\\PPSS) }} Method (_PSD, 0, NotSerialized) {{ Return (\\PPSD) }} }}\n}}\n'''
        add(f'PstatePair{pair}', f'PSTATE-PAIR-{pair}', f'SSDT-PSTATE-PAIR-{pair}.dsl', methods)
        partial = f'''DefinitionBlock ("", "SSDT", 2, "BC25", "C2PAIR{pair}", 1)\n{{\n{cstate_ext}\n    Scope (\\_PR.P{p}) {{ Method (_CST, 0, NotSerialized) {{ Return (\\PCST ()) }} Name (_CSD, Package () {{ Package () {{ 6, 0, 0x{pair:08X}, 0xFE, 2, 0 }} }}) }}\n    Scope (\\_PR.P{q}) {{ Method (_CST, 0, NotSerialized) {{ Return (\\PCST ()) }} Name (_CSD, Package () {{ Package () {{ 6, 0, 0x{pair:08X}, 0xFE, 2, 0 }} }}) }}\n}}\n'''
        add(f'PartialCstatePair{pair}', f'CSTATE-PAIR-2-{pair}', f'SSDT-CSTATE-PAIR-2-{pair}.dsl', partial)
        full = f'''DefinitionBlock ("", "SSDT", 2, "BC25", "C3PAIR{pair}", 1)\n{{\n{cstate_ext}\n    Scope (\\_PR.P{p}) {{ Method (_CST, 0, NotSerialized) {{ Return (\\PCST ()) }} }}\n    Scope (\\_PR.P{q}) {{ Method (_CST, 0, NotSerialized) {{ Return (\\PCST ()) }} }}\n}}\n'''
        add(f'FullCstatePair{pair}', f'CSTATE-PAIR-3-{pair}', f'SSDT-CSTATE-PAIR-3-{pair}.dsl', full)

    tables.append(('Ptswak', 'SSDT-PTSWAK', TABLE_ROOT / 'Common' / 'SSDT-PTSWAK.dsl'))
    return tables


def compile_table(iasl: str, source: Path, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [iasl, '-p', str(out_dir / source.stem), str(source)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    aml = out_dir / f'{source.stem}.aml'
    if not aml.exists():
        raise SystemExit(f'Missing AML output for {source}')
    return aml


def format_array(name: str, data: bytes) -> str:
    lines = []
    for idx in range(0, len(data), 12):
        chunk = ', '.join(f'0x{byte:02X}' for byte in data[idx:idx + 12])
        lines.append(f'  {chunk}')
    return f'static CONST UINT8 g{name}[] = {{\n' + ',\n'.join(lines) + '\n};\n'


def main() -> int:
    iasl = shutil.which('iasl')
    if iasl is None:
        print('error: iasl not found on PATH', file=sys.stderr)
        return 1

    tables = write_generated_sources()
    compiled = {}
    for symbol, _, source in tables:
        compiled[symbol] = compile_table(iasl, source, BUILD_ROOT / source.parent.name).read_bytes()

    header = [
        '#ifndef BC250_DXEV3_ACPI_AUTO_INJECT_BLOBS_H',
        '#define BC250_DXEV3_ACPI_AUTO_INJECT_BLOBS_H',
        '',
        '#include <Uefi.h>',
        '',
        '#define BC250_MAX_ACPI_TABLES 32',
        '',
        'typedef struct {',
        '  CONST CHAR8   *Name;',
        '  CONST UINT8   *Data;',
        '  UINTN         Size;',
        '} BC250_ACPI_BLOB;',
        '',
    ]

    arrays = [format_array(symbol, compiled[symbol]) for symbol, _, _ in tables]

    def blob(symbol: str, name: str) -> str:
        return f'  {{ "{name}", g{symbol}, sizeof (g{symbol}) }},'

    footer = [
        'static CONST BC250_ACPI_BLOB gPstateCommonTable = { "PSTATE-COMMON", gPstateCommon, sizeof (gPstateCommon) };',
        'static CONST BC250_ACPI_BLOB gPartialCstateCommonTable = { "CSTATE-COMMON-2", gPartialCstateCommon, sizeof (gPartialCstateCommon) };',
        'static CONST BC250_ACPI_BLOB gFullCstateCommonTable = { "CSTATE-COMMON-3", gFullCstateCommon, sizeof (gFullCstateCommon) };',
        'static CONST BC250_ACPI_BLOB gPtswakTable = { "SSDT-PTSWAK", gPtswak, sizeof (gPtswak) };',
        'static CONST BC250_ACPI_BLOB gPstatePairs[] = {',
    ] + [blob(f'PstatePair{i}', f'PSTATE-PAIR-{i}') for i in range(8)] + [
        '};', 'static CONST BC250_ACPI_BLOB gPartialCstatePairs[] = {',
    ] + [blob(f'PartialCstatePair{i}', f'CSTATE-PAIR-2-{i}') for i in range(8)] + [
        '};', 'static CONST BC250_ACPI_BLOB gFullCstatePairs[] = {',
    ] + [blob(f'FullCstatePair{i}', f'CSTATE-PAIR-3-{i}') for i in range(8)] + [
        '};', '#define BC250_PAIR_TABLE_COUNT 8', '', '#endif', ''
    ]

    OUT_HEADER.write_text('\n'.join(header) + ''.join(arrays) + '\n'.join(footer))
    print(f'wrote {OUT_HEADER}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
