# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software distributed
# under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.
"""Differential FTS correctness checks against an official baseline runtime.

Uses real TOKENIZE and MATCH execution. The default sweep passes every Unicode
scalar value through IK in consecutive 512-character chunks, preserving exact
JSON array order and token frequencies. Surrogates are intentionally excluded
from Unicode scalar values and malformed bytes are tested separately.
Requires PyMySQL and two isolated test seekdb instances.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-port', type=int, default=12883)
    parser.add_argument('--modified-port', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--skip-unicode-sweep', action='store_true')
    args = parser.parse_args()
    connections = [pymysql.connect(host='127.0.0.1', port=port, user='root',
                                    charset='utf8mb4', autocommit=True,
                                    read_timeout=120)
                   for port in (args.baseline_port, args.modified_port)]
    checks = []

    def query(connection, sql, values=None):
        with connection.cursor() as cursor:
            cursor.execute(sql, values)
            return cursor.fetchall()

    def digest(value):
        return hashlib.sha256(json.dumps(value, ensure_ascii=False, default=str).encode()).hexdigest()

    def compare(name, sql, values=None, expected_error=False):
        outputs = []
        for connection in connections:
            try:
                outputs.append(dict(rows=query(connection, sql, values)))
            except pymysql.MySQLError as error:
                outputs.append(dict(error_code=error.args[0]))
        passed = outputs[0] == outputs[1]
        passed = passed and all(('error_code' in output) == expected_error for output in outputs)
        record = dict(name=name, passed=passed, baseline_sha256=digest(outputs[0]),
                      modified_sha256=digest(outputs[1]))
        if not passed:
            record['outputs'] = [str(output)[:800] for output in outputs]
            record['sql'] = sql
        checks.append(record)
        return passed

    corpus = [
        '', None, 'a', '一', 'hello world', 'HELLO Hello hello WORLD',
        'OceanBase全文索引与数据库分词测试，稳定可靠！',
        '中华人民共和国 长江大桥 南京市长江大桥 研究生命起源',
        '1 12.34 1,234.56 一二三十百千万亿 １２３４５ 六十公斤 20kg',
        'a_b a-b a.b a@b a+b a&b a#b _leading trailing_ ... ,,,\t\r\n',
        '㐀\u4dbf\u4e00\u9fff\uf900\ufaff\u3040あアカタカナ한글\uff00\uffef',
        '🌊🚀𠀀\U0010ffffemoji ASCII １２３ ＡＢＣ abc',
        "quote' double\" slash\\ percent% zero\x00 end",
        'the and or for with TO A an of repeated repeated repeated 的 了 和 与 我们',
        ('数据库全文索引 database tokenizer repeat 123.45公斤 ' * 45),
    ]
    for index, text in enumerate(corpus):
        for tokenizer in ('ik', 'beng', 'space', 'ngram', 'ngram2'):
            for output in ('default', 'all'):
                compare('corpus_%d_%s_%s' % (index, tokenizer, output),
                        'SELECT TOKENIZE(%s,%s,%s) AS parsed',
                        (text, tokenizer, json.dumps([dict(output=output)])),
                        # Untyped NULL has no supported IK dictionary charset.
                        expected_error=text is None and tokenizer == 'ik')

    for mode in ('smart', 'max_word'):
        compare('ik_mode_' + mode,
                'SELECT TOKENIZE(%s,%s,%s) AS parsed',
                ('研究生命起源南京市长江大桥１２３公里', 'ik', json.dumps([
                    dict(output='all'), dict(additional_args=[dict(ik_mode=mode)])])))
    for charset in ('utf16', 'utf16le'):
        # IK dictionaries currently reject these encodings. Keep that existing
        # error behavior while optimizing classification helpers shared by them.
        compare('unsupported_ik_charset_' + charset,
                'SELECT TOKENIZE(CONVERT(%s USING ' + charset + "),'ik') AS parsed",
                ('中文abc123',), expected_error=True)
    for raw in ('80', 'C0AF', 'E08080', 'F0808080', 'F4908080', 'E4B8'):
        compare('malformed_utf8_' + raw,
                "SELECT TOKENIZE(CAST(0x" + raw + " AS CHAR CHARACTER SET utf8mb4),'ik') AS parsed",
                expected_error=True)

    scalar_count = 0
    if not args.skip_unicode_sweep:
        chunk = []
        first = 0
        for codepoint in range(0x110000):
            if 0xD800 <= codepoint <= 0xDFFF:
                continue
            if not chunk:
                first = codepoint
            chunk.append(chr(codepoint))
            scalar_count += 1
            if len(chunk) == 512 or codepoint == 0x10FFFF:
                compare('unicode_scalars_%06x_%06x' % (first, codepoint),
                        "SELECT TOKENIZE(%s,'ik','[{\"output\":\"all\"}]') AS parsed",
                        (''.join(chunk),))
                chunk = []

    def concurrent_worker(worker):
        local_connections = [pymysql.connect(host='127.0.0.1',port=port,user='root',
                                              charset='utf8mb4',autocommit=True,read_timeout=120)
                             for port in (args.baseline_port,args.modified_port)]
        result = dict(name='concurrent_worker_' + str(worker),passed=True,queries=0)
        try:
            for iteration in range(25):
                text = ('worker%d_%d 数据库数据库１２３公斤 Alpha alpha ' % (worker,iteration)) * (1+iteration%3)
                outputs = [query(connection,"SELECT TOKENIZE(%s,'ik','[{\"output\":\"all\"}]') AS parsed",(text,))
                           for connection in local_connections]
                if outputs[0] != outputs[1]:
                    result['passed'] = False
                    result['iteration'] = iteration
                    break
                result['queries'] += 1
        except Exception as error:
            result.update(passed=False,error=str(error))
        finally:
            for connection in local_connections:
                connection.close()
        return result
    with ThreadPoolExecutor(max_workers=8) as executor:
        checks.extend(executor.map(concurrent_worker,range(8)))

    # Separate database: never alter the official performance benchmark schema.
    for connection in connections:
        query(connection, 'DROP DATABASE IF EXISTS fts_opt_regression')
        query(connection, 'CREATE DATABASE fts_opt_regression')
        query(connection, 'USE fts_opt_regression')
        query(connection, 'CREATE TABLE docs(id INT PRIMARY KEY, body VARCHAR(2048), en VARCHAR(2048))')
        rows = [(i, corpus[6 + i % 6], corpus[3 + i % 3] + ' database tokenizer audit')
                for i in range(1, 81)]
        with connection.cursor() as cursor:
            cursor.executemany('INSERT INTO docs VALUES(%s,%s,%s)', rows)
        query(connection, 'CREATE FULLTEXT INDEX ik_idx ON docs(body) WITH PARSER ik')
        query(connection, 'CREATE FULLTEXT INDEX beng_idx ON docs(en) WITH PARSER beng')

    def query_matches(stage):
        for column, needle in [('body', '数据库 全文索引'), ('body', '中华人民共和国 长江'),
                               ('body', '123 公斤 OceanBase'), ('en', 'database audit'),
                               ('en', 'hello tokenizer'), ('body', 'notexistingterm')]:
            for boolean in (False, True):
                clause = ' IN BOOLEAN MODE' if boolean else ''
                sql = 'SELECT id,MATCH(' + column + ') AGAINST(%s' + clause + ') AS score FROM docs '
                sql += 'WHERE MATCH(' + column + ') AGAINST(%s' + clause + ') ORDER BY id'
                compare(stage + '_' + column + '_' + needle + '_' + str(boolean), sql, (needle, needle))

    query_matches('built_index')
    def concurrent_index_worker(worker):
        local_connections = [pymysql.connect(host='127.0.0.1',port=port,user='root',
                                              database='fts_opt_regression',charset='utf8mb4',
                                              autocommit=True,read_timeout=120)
                             for port in (args.baseline_port,args.modified_port)]
        result = dict(name='concurrent_match_worker_' + str(worker),passed=True,queries=0)
        try:
            needles = [('body','数据库 全文索引'),('body','123 公斤'),('en','hello tokenizer'),('en','database audit')]
            for iteration in range(25):
                column,needle = needles[(worker+iteration)%len(needles)]
                mode = ' IN BOOLEAN MODE' if iteration%2 else ''
                sql = 'SELECT id,MATCH(' + column + ') AGAINST(%s' + mode + ') AS score FROM docs '
                sql += 'WHERE MATCH(' + column + ') AGAINST(%s' + mode + ') AND MOD(id,3)=%s ORDER BY id'
                outputs = [query(connection,sql,(needle,needle,worker%3)) for connection in local_connections]
                if outputs[0] != outputs[1]:
                    result.update(passed=False,iteration=iteration)
                    break
                result['queries'] += 1
        except Exception as error:
            result.update(passed=False,error=str(error))
        finally:
            for connection in local_connections:
                connection.close()
        return result
    with ThreadPoolExecutor(max_workers=8) as executor:
        checks.extend(executor.map(concurrent_index_worker,range(8)))
    for connection in connections:
        query(connection, 'UPDATE docs SET body=%s,en=%s WHERE MOD(id,5)=0',
              ('更新后的数据库全文检索 repeat repeat repeat', 'changed database audit repeat repeat'))
        query(connection, 'DELETE FROM docs WHERE MOD(id,7)=0')
        query(connection, 'INSERT INTO docs VALUES(99,%s,%s)',
              ('全新数据库全文索引', 'new database audit'))
    query_matches('after_dml')
    for connection in connections:
        query(connection, 'DROP INDEX ik_idx ON docs')
        query(connection, 'CREATE FULLTEXT INDEX ik_idx ON docs(body) WITH PARSER ik')
    query_matches('rebuilt_index')
    compare('table_integrity', 'SELECT id,body,en FROM docs ORDER BY id')

    for connection in connections:
        connection.close()
    report = dict(utc=datetime.now(timezone.utc).isoformat(),
                  baseline_port=args.baseline_port, modified_port=args.modified_port,
                  unicode_scalar_count=scalar_count,
                  passed=sum(check['passed'] for check in checks), total=len(checks),
                  official_evaluation_submitted=False, checks=checks)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=report['passed'], total=report['total'],
                          unicode_scalar_count=scalar_count,
                          failed=[check for check in checks if not check['passed']],
                          evidence=str(args.output)), ensure_ascii=False))
    return 0 if all(check['passed'] for check in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
