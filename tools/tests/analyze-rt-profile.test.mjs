import { strict as assert } from 'node:assert';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

const analyzer = fileURLToPath(new URL('../analyze-rt-profile.mjs', import.meta.url));
const fixture = fileURLToPath(new URL('./rt-profile-sample.log', import.meta.url));

test('pairs both orders and rejects changed geometry', () => {
    const result = spawnSync(process.execPath, [analyzer, fixture], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /pairs=2/);
    assert.match(result.stdout, /baselineMedianMs=5 variantMedianMs=2\.5 pairedMedianDeltaMs=2\.5/);
});

test('honors a minimum frame', () => {
    const result = spawnSync(process.execPath, [analyzer, fixture, '2', '300'], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /pairs=1/);
    assert.match(result.stdout, /pairedMedianDeltaMs=3/);
});
