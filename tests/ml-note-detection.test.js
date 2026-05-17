// Integration smoke test for the ML note detection addon path.
//
// Loads the built native addon directly (no Electron), verifies the Basic
// Pitch model loads through loadNoteModel(), that isMlNoteDetection() reports
// the ML path, that getPitchDetection()/scoreChord() keep their shapes, and
// that a bad model path fails soft to the YIN fallback (Constitution VII).
//
// Run:  node --test tests/ml-note-detection.test.js
// Skips cleanly when the addon hasn't been built yet (npm run build:audio).
//
// WAV→MIDI detection accuracy is covered separately by the Phase 0 spike
// (tests/spike/) which exercises the identical model + ONNX Runtime.

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');

const ADDON = path.join(__dirname, '..', 'build', 'Release', 'slopsmith_audio.node');
const MODEL = path.join(__dirname, '..', 'resources', 'models', 'basic_pitch.onnx');

let audio = null;
try {
    audio = require(ADDON);
} catch (e) {
    if (!fs.existsSync(ADDON)) {
        // Addon genuinely not built yet — skip cleanly.
        test('ml-note-detection (skipped — addon not built)', { skip: true }, () => {});
    } else {
        // The addon file IS present but failed to load — a real staging
        // regression (e.g. a missing/incompatible ONNX Runtime library next
        // to it). Surface it as a failure so this smoke test catches it.
        test('ml-note-detection addon loads', () => { throw e; });
    }
}

if (audio) {
    test('ML note detection addon path', async (t) => {
        audio.init();
        t.after(() => { try { audio.shutdown(); } catch { /* ignore */ } });

        await t.test('loadNoteModel loads the bundled Basic Pitch model', () => {
            assert.equal(typeof audio.loadNoteModel, 'function',
                'addon exposes loadNoteModel');
            assert.ok(fs.existsSync(MODEL), `model present at ${MODEL}`);
            const ok = audio.loadNoteModel(MODEL);
            // ok is false if ONNX support was compiled out — tolerate that,
            // but then isMlNoteDetection must agree.
            assert.equal(typeof ok, 'boolean');
            assert.equal(audio.isMlNoteDetection(), ok,
                'isMlNoteDetection agrees with the load result');
        });

        await t.test('loadNoteModel fails soft on a missing file', () => {
            // loadNoteModel returns "is ML available after this call". A
            // missing file never throws and never tears down a model that was
            // already loaded, so the result stays consistent with
            // isMlNoteDetection().
            const before = audio.isMlNoteDetection();
            const ok = audio.loadNoteModel(path.join(__dirname, 'no-such-model.onnx'));
            assert.equal(typeof ok, 'boolean', 'missing model returns a boolean, does not throw');
            assert.equal(audio.isMlNoteDetection(), before,
                'a failed load must not change ML availability');
            assert.equal(ok, before,
                'return value reflects post-call availability');
        });

        await t.test('getPitchDetection keeps its shape', () => {
            const d = audio.getPitchDetection();
            assert.equal(typeof d, 'object');
            for (const k of ['frequency', 'confidence', 'midiNote', 'cents', 'noteName'])
                assert.ok(k in d, `detection has ${k}`);
        });

        await t.test('scoreChord keeps its shape (no device running)', () => {
            const res = audio.scoreChord({
                arrangement: 'guitar',
                stringCount: 6,
                offsets: [0, 0, 0, 0, 0, 0],
                notes: [{ s: 0, f: 3 }, { s: 1, f: 2 }, { s: 2, f: 0 }],
            });
            assert.ok(res && typeof res === 'object', 'scoreChord returns an object');
            for (const k of ['score', 'hitStrings', 'totalStrings', 'isHit', 'results'])
                assert.ok(k in res, `result has ${k}`);
            assert.equal(res.results.length, 3, 'one result entry per note');
        });

        await t.test('detectNotes returns the polyphonic shape or null', () => {
            assert.equal(typeof audio.detectNotes, 'function',
                'addon exposes detectNotes');
            const res = audio.detectNotes();
            // null when ONNX support is compiled out / no model — otherwise a
            // { notes: [], sampleRate } object.
            if (res !== null) {
                assert.ok('notes' in res && Array.isArray(res.notes),
                    'detectNotes result has a notes array');
                assert.equal(typeof res.sampleRate, 'number',
                    'detectNotes result has a numeric sampleRate');
            }
        });
    });
}
