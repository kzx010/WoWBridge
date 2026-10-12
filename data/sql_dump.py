"""Read selected tables from a mysqldump without a database or executing SQL."""
import gzip
import re
from pathlib import Path


# mysqldump values: quoted strings with MySQL backslash escapes, NULL, numbers.
TOKEN = re.compile(r"'(?:[^'\\]|\\.|'')*'|NULL|[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?|[(),;]", re.DOTALL)


def tables(path: Path, wanted: dict[str, tuple[str, ...] | None], filters=None) -> dict[str, list[dict]]:
    """Select columns by CREATE TABLE names, never by hard-coded SQL positions.

    None selects every column. Optional per-table predicates retain selected rows.
    The supported input is the standard single-line extended INSERT format used
    by the ClassicDB full dump. Refuse missing tables/columns and truncated rows.
    """
    result = {name: [] for name in wanted}
    schemas = {}
    current = None
    opener = gzip.open if str(path).endswith('.gz') else open
    with opener(path, 'rt', encoding='utf-8') as source:
        for line in source:
            if line.startswith('CREATE TABLE `'):
                current = line.split('`')[1]
                if current in wanted:
                    schemas[current] = []
            elif current in wanted and line.lstrip().startswith('`'):
                schemas[current].append(line.split('`')[1])
            elif line.startswith(') ENGINE'):
                current = None
            if not line.startswith('INSERT INTO `'):
                continue
            name = line.split('`')[1]
            if name not in wanted:
                continue
            columns = schemas[name]
            selected = {columns.index(c): c for c in (wanted[name] or columns)}
            row, position, in_row = {}, 0, False
            for match in TOKEN.finditer(line.split(' VALUES ', 1)[1]):
                value = match.group()
                if value == '(':
                    row, position, in_row = {}, 0, True
                elif value == ')':
                    if position != len(columns):
                        raise ValueError(f'{name}: row has {position} values, expected {len(columns)}')
                    if not filters or name not in filters or filters[name](row):
                        result[name].append(row)
                    in_row = False
                elif in_row and value != ',':
                    if position in selected:
                        if value == 'NULL':
                            parsed = None
                        elif value.startswith("'"):
                            parsed = re.sub(r'\\(.)', lambda m: {'n': '\n', 'r': '\r', 't': '\t', '0': '\0'}.get(m[1], m[1]), value[1:-1]).replace("''", "'")
                        else:
                            parsed = float(value) if any(c in value for c in '.eE') else int(value)
                        row[selected[position]] = parsed
                    position += 1
            if in_row:
                raise ValueError(f'{name}: incomplete INSERT')
    missing = set(wanted) - schemas.keys()
    if missing:
        raise ValueError(f'missing dump tables: {sorted(missing)}')
    return result
