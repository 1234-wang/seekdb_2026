#!/usr/bin/env python3
"""Check UUID SQL on an owned local instance; preserve failed SQL contracts."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import uuid
import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--port', type=int, default=18340)
    parser.add_argument('--mode', choices=['normal', 'eperm', 'empty'], required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 18340 <= args.port <= 18349 or args.output.exists():
        parser.error('Require a systems-owned port and a new output file')
    result = {'mode': args.mode, 'checks': {}, 'fatal': None}
    def connect():
        return pymysql.connect(host='127.0.0.1', port=args.port, unix_socket=args.socket,
            user='root', database='oceanbase', charset='utf8mb4', autocommit=True,
            connect_timeout=5, read_timeout=15)
    def repeated(_):
        with connect() as connection, connection.cursor() as cursor:
            values = []
            for unused in range(100):
                cursor.execute('select uuid()')
                values.append(cursor.fetchone()[0])
            return values
    try:
        sequential = repeated(0)
        with ThreadPoolExecutor(max_workers=4) as pool:
            parallel = [value for batch in pool.map(repeated, range(4)) for value in batch]
        values = sequential + parallel
        parsed = [uuid.UUID(value) for value in values]
        nodes = {value.node for value in parsed}
        hardware = {int(p.read_text().strip().replace(':', ''), 16)
                    for p in Path('/sys/class/net').glob('*/address')
                    if p.parent.name != 'lo'}
        result.update(sequential=sequential, concurrent=parallel,
                      nodes=[f'{node:012x}' for node in sorted(nodes)],
                      hardware_nodes=[f'{node:012x}' for node in sorted(hardware)])
        checks = result['checks']
        checks['version_one'] = all(value.version == 1 for value in parsed)
        checks['rfc_variant'] = all(value.variant == uuid.RFC_4122 for value in parsed)
        checks['separate_statement_unique_500'] = len(values) == len(set(values)) == 500
        checks['stable_node_within_process'] = len(nodes) == 1
        checks['expected_node_source'] = (nodes <= hardware if args.mode == 'normal'
            else all(node & (1 << 40) for node in nodes))
        with connect() as connection, connection.cursor() as cursor:
            cursor.execute('select uuid() as first_value, uuid() as second_value')
            pair = list(cursor.fetchone())
            result['same_statement'] = {'sql': 'select uuid(), uuid()', 'values': pair,
                                        'expected_distinct': True, 'passed': pair[0] != pair[1]}
            sql = 'select uuid() from (' + ' union all '.join(
                'select ' + str(i) + ' as n' for i in range(8)) + ') q'
            cursor.execute(sql)
            rows = [row[0] for row in cursor.fetchall()]
            result['multirow'] = {'sql': sql, 'values': rows, 'expected_distinct': 8,
                                  'passed': len(rows) == len(set(rows)) == 8}
        # Keep independent UUID expression findings visible. The runner compares
        # them to the unchanged baseline; they never become passing assertions.
        result['all_sql_checks_passed'] = (all(checks.values())
            and result['same_statement']['passed'] and result['multirow']['passed'])
        result['node_and_repeated_query_checks_passed'] = all(checks.values())
    except Exception as exc:
        result['fatal'] = repr(exc)
        result['node_and_repeated_query_checks_passed'] = False
        result['all_sql_checks_passed'] = False
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps({key: value for key, value in result.items()
        if key in ['checks', 'fatal', 'all_sql_checks_passed',
                   'node_and_repeated_query_checks_passed']}, indent=2), flush=True)
    return int(not result['node_and_repeated_query_checks_passed'])


if __name__ == '__main__':
    raise SystemExit(main())
