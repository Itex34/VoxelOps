import { readFileSync } from 'node:fs';

const [path, minFrameArg = '0', lastPairsArg = '300'] = process.argv.slice(2);
const minFrame = Number(minFrameArg);
const lastPairs = Number(lastPairsArg);
if (!path || !Number.isInteger(minFrame) || minFrame < 0 ||
    !Number.isInteger(lastPairs) || lastPairs < 1) {
    console.error('Usage: node tools/analyze-rt-profile.mjs <log> [min-frame] [last-pairs]');
    process.exit(1);
}

const pattern = /\[Vulkan\]\[RT profile\] frame=(\d+) stage=(\d+) variant=([01]) triangles=(\d+) chunkMs=([\d.]+) gpuFrameMs=([\d.]+)/;
const groups = new Map();
const bytes = readFileSync(path);
// Windows PowerShell 5 may redirect native output as UTF-16LE.
const encoding = bytes[0] === 0xff && bytes[1] === 0xfe ? 'utf16le' : 'utf8';
for (const line of bytes.toString(encoding).split(/\r?\n/)) {
    const match = pattern.exec(line);
    if (!match) continue;
    const [, frameText, stageText, variantText, trianglesText, chunkText] = match;
    const frame = Number(frameText);
    if (frame < minFrame) continue;
    const key = Math.floor(frame / 2);
    const group = groups.get(key) ?? [];
    group.push({ frame, stage: Number(stageText), variant: Number(variantText),
                 triangles: Number(trianglesText), chunkMs: Number(chunkText) });
    groups.set(key, group);
}

const pairs = [];
for (const group of groups.values()) {
    const baseline = group.filter(record => record.variant === 0);
    const variant = group.filter(record => record.variant === 1);
    if (baseline.length !== 1 || variant.length !== 1 ||
        baseline[0].triangles !== variant[0].triangles) continue;
    pairs.push({ ...baseline[0], variantMs: variant[0].chunkMs,
                 delta: baseline[0].chunkMs - variant[0].chunkMs });
}
pairs.sort((a, b) => a.frame - b.frame);
const selected = pairs.slice(-lastPairs);
if (!selected.length) throw new Error('No complete adjacent baseline/variant pairs found.');
const stageNames = { 1: 'primary_sun', 2: 'indirect', 3: 'secondary_sun', 4: 'second_bounce' };
const median = values => {
    values.sort((a, b) => a - b);
    const mid = Math.floor(values.length / 2);
    return values.length % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
};
console.log(`stage=${stageNames[selected[0].stage] ?? selected[0].stage} pairs=${selected.length} frames=${selected[0].frame}-${selected.at(-1).frame} triangles=${selected.at(-1).triangles}`);
console.log(`baselineMedianMs=${median(selected.map(p => p.chunkMs))} variantMedianMs=${median(selected.map(p => p.variantMs))} pairedMedianDeltaMs=${median(selected.map(p => p.delta))}`);
console.log('Ablations remove related work and may change scheduling; deltas are not additive ray-query timings.');
