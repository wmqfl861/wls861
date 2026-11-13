"""Explicit v3 copies of legacy FakeModel fixtures; original files stay intact."""
import copy
import json
import sqlite3
from contextlib import closing


def legacy_context(source, request_format='framed-v1'):
    for line in ('  tool_policy: fixed-role-v1\n', '  model_projection: lean-v1\n',
                 '  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n'):
        source = source.replace(line, '')
    return source.replace('request_format: structured-v3', 'request_format: ' + request_format)


def v3_fake_script(value):
    value = copy.deepcopy(value)
    def visit(item):
        if isinstance(item, list):
            for child in item:
                visit(child)
        elif isinstance(item, dict):
            if item.get('name') in ('submit_plan', 'submit_amendment_plan'):
                args = item['arguments']
                plan = args if item['name'] == 'submit_plan' else args['plan']
                for node in plan['nodes']:
                    node['brief']['title'] = node['title']
            for child in item.values():
                visit(child)
    visit(value)
    return value


def frozen_script(database, request, fixture, directory):
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as db:
        snapshot, = db.execute('SELECT config_snapshot FROM requests WHERE request_id=?', (request,)).fetchone()
    if json.loads(snapshot)['system']['context'].get('request_format', 'framed-v1') != 'structured-v3':
        return fixture
    derived = directory / ('v3-' + fixture.name)
    derived.write_text(json.dumps(v3_fake_script(json.loads(fixture.read_text(encoding='utf-8')))), encoding='utf-8')
    return derived
