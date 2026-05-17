// Slopsmith Audio Engine Plugin — Frontend
// Communicates with the JUCE audio engine via window.slopsmithDesktop.audio
// Desktop audio engine plugin

window.__slopsmithDesktopAudioHooks = window.__slopsmithDesktopAudioHooks || {};

(function() {
    'use strict';

    const api = window.slopsmithDesktop?.audio;
    if (!api) {
        console.error('[audio-engine] Desktop audio API not available — running in browser mode');
        const panel = document.getElementById('audio-engine-panel');
        if (panel) panel.innerHTML = '<div class="p-8 text-center text-slate-400">Audio engine is only available in the Slopsmith Desktop app.</div>';
        return;
    }

    // Hook registry survives renderer re-evaluation; interval IDs live here so
    // the next stopToneMonitor call (from a re-bound playSong wrapper) can
    // cancel a timer started by the previous evaluation. Don't preemptively
    // clear here — letting the prior interval keep running preserves mid-song
    // tone polling, since its closure refs (toneSwitcher, autoSwitchEnabled)
    // are still valid until the next playSong rotates to the new closure.
    const hookState = window.__slopsmithDesktopAudioHooks;

    // ── State ─────────────────────────────────────────────────────────────────
    let audioRunning = false;
    let meterAnimFrame = null;
    let knownPlugins = [];
    let currentDeviceTypes = [];

    // ── Elements ──────────────────────────────────────────────────────────────
    const $ = (id) => document.getElementById(id);

    const statusDot = $('ae-status-dot');
    const statusText = $('ae-status-text');
    const latencyEl = $('ae-latency');
    const toggleBtn = $('ae-toggle');
    const deviceTypeSelect = $('ae-device-type');
    const inputDeviceSelect = $('ae-input-device');
    const outputDeviceSelect = $('ae-output-device');
    const sampleRateSelect = $('ae-sample-rate');
    const bufferSizeSelect = $('ae-buffer-size');
    const inputChannelSelect = $('ae-input-channel');
    const applyDeviceBtn = $('ae-apply-device');
    const meterInput = $('ae-meter-input');
    const meterOutput = $('ae-meter-output');
    const inputGainSlider = $('ae-input-gain');
    const outputGainSlider = $('ae-output-gain');
    const inputGainLabel = $('ae-input-gain-label');
    const outputGainLabel = $('ae-output-gain-label');
    const monitorMuteCheckbox = $('ae-monitor-mute');
    const chainContainer = $('ae-chain');
    const addVstBtn = $('ae-add-vst');
    const addNamBtn = $('ae-add-nam');
    const addIrBtn = $('ae-add-ir');
    const clearChainBtn = $('ae-clear-chain');
    const vstBrowser = $('ae-vst-browser');
    const scanVstsBtn = $('ae-scan-vsts');
    const vstSearch = $('ae-vst-search');
    const vstList = $('ae-vst-list');
    const pitchNote = $('ae-pitch-note');
    const pitchFreq = $('ae-pitch-freq');
    const pitchCentsBar = $('ae-pitch-cents');
    const savePresetBtn = $('ae-save-preset');
    const noiseGateEnable = $('ae-noise-gate-enable');
    const noiseGateThresholdWrap = $('ae-noise-gate-threshold-wrap');
    const noiseGateThresholdSlider = $('ae-noise-gate-threshold');
    const noiseGateThresholdLabel = $('ae-noise-gate-threshold-label');
    const noiseGateReleaseSlider = $('ae-noise-gate-release');
    const noiseGateReleaseLabel = $('ae-noise-gate-release-label');
    const noiseGateDepthSlider = $('ae-noise-gate-depth');
    const noiseGateDepthLabel = $('ae-noise-gate-depth-label');

    /** Sliders show dB; `api.setGain` and saved presets use linear amplitude gain (legacy presets unchanged). */
    const GAIN_SLIDER_DB_MIN = -60;
    const GAIN_SLIDER_DB_MAX = 12;

    function linearGainToDb(lin) {
        const x = Number(lin);
        if (!Number.isFinite(x) || x <= 0) return GAIN_SLIDER_DB_MIN;
        const db = 20 * Math.log10(x);
        return Math.min(GAIN_SLIDER_DB_MAX, Math.max(GAIN_SLIDER_DB_MIN, db));
    }

    function dbToLinearGain(db) {
        const x = Number(db);
        if (!Number.isFinite(x)) return 1;
        const clamped = Math.min(GAIN_SLIDER_DB_MAX, Math.max(GAIN_SLIDER_DB_MIN, x));
        // Always convert via the formula — returning 0 at GAIN_SLIDER_DB_MIN (-60 dB) would
        // produce a true mute instead of the ~0.001 linear gain that -60 dB represents.
        return Math.pow(10, clamped / 20);
    }

    function formatGainDbLabel(db) {
        const x = Number(db);
        if (!Number.isFinite(x)) return '0.0 dB';
        return `${x.toFixed(1)} dB`;
    }

    window._aeLinearGainToDb = linearGainToDb;
    window._aeDbToLinearGain = dbToLinearGain;
    window._aeFormatGainDbLabel = formatGainDbLabel;

    // ── Persistence ─────────────────────────────────────────────────────────
    function saveDeviceSettings() {
        localStorage.setItem('slopsmith-audio-device', JSON.stringify({
            type: deviceTypeSelect.value,
            input: inputDeviceSelect.value,
            output: outputDeviceSelect.value,
            sampleRate: sampleRateSelect.value,
            bufferSize: bufferSizeSelect.value,
            inputChannel: inputChannelSelect.value,
            monitorMute: monitorMuteCheckbox.checked,
        }));
    }

    function loadDeviceSettings() {
        try {
            const raw = localStorage.getItem('slopsmith-audio-device');
            return raw ? JSON.parse(raw) : null;
        } catch { return null; }
    }

    // ── Noise gate (AmpliTube-style: threshold, release ms, depth dB → native setNoiseGate) ──
    const AE_NOISE_GATE_THRESHOLD_MIN = -96;
    const AE_NOISE_GATE_THRESHOLD_MAX = 0;
    const AE_NOISE_GATE_THRESHOLD_DEFAULT = -60;
    const AE_NOISE_GATE_RELEASE_MIN = 5;
    const AE_NOISE_GATE_RELEASE_MAX = 2000;
    const AE_NOISE_GATE_RELEASE_DEFAULT = 100;
    const AE_NOISE_GATE_DEPTH_MIN = -100;
    const AE_NOISE_GATE_DEPTH_MAX = 0;
    const AE_NOISE_GATE_DEPTH_DEFAULT = -60;

    function aeClampNoiseGateThresholdDb(db) {
        const x = Number(db);
        const v = Number.isFinite(x) ? x : AE_NOISE_GATE_THRESHOLD_DEFAULT;
        return Math.min(AE_NOISE_GATE_THRESHOLD_MAX, Math.max(AE_NOISE_GATE_THRESHOLD_MIN, v));
    }

    function aeClampNoiseGateReleaseMs(ms) {
        const x = Number(ms);
        const v = Number.isFinite(x) ? x : AE_NOISE_GATE_RELEASE_DEFAULT;
        const stepped = Math.round(v / 5) * 5;
        return Math.min(AE_NOISE_GATE_RELEASE_MAX, Math.max(AE_NOISE_GATE_RELEASE_MIN, stepped));
    }

    function aeClampNoiseGateDepthDb(db) {
        const x = Number(db);
        const v = Number.isFinite(x) ? x : AE_NOISE_GATE_DEPTH_DEFAULT;
        return Math.min(AE_NOISE_GATE_DEPTH_MAX, Math.max(AE_NOISE_GATE_DEPTH_MIN, v));
    }

    function aeSyncNoiseGateThresholdLabel() {
        if (!noiseGateThresholdLabel || !noiseGateThresholdSlider) return;
        const db = parseFloat(noiseGateThresholdSlider.value);
        noiseGateThresholdLabel.textContent = (Number.isFinite(db) ? db.toFixed(0) : String(AE_NOISE_GATE_THRESHOLD_DEFAULT)) + ' dB';
    }

    function aeSyncNoiseGateReleaseLabel() {
        if (!noiseGateReleaseLabel || !noiseGateReleaseSlider) return;
        const ms = parseFloat(noiseGateReleaseSlider.value);
        noiseGateReleaseLabel.textContent = (Number.isFinite(ms) ? String(Math.round(ms)) : String(AE_NOISE_GATE_RELEASE_DEFAULT)) + ' ms';
    }

    function aeSyncNoiseGateDepthLabel() {
        if (!noiseGateDepthLabel || !noiseGateDepthSlider) return;
        const db = parseFloat(noiseGateDepthSlider.value);
        noiseGateDepthLabel.textContent = (Number.isFinite(db) ? db.toFixed(0) : String(AE_NOISE_GATE_DEPTH_DEFAULT)) + ' dB';
    }

    function aeSyncNoiseGatePanelVisibility() {
        if (!noiseGateThresholdWrap || !noiseGateEnable) return;
        noiseGateThresholdWrap.style.display = noiseGateEnable.checked ? '' : 'none';
    }

    /** Preset serialization — stored next to inputGain/outputGain on each chain preset. */
    function captureCurrentNoiseGateState() {
        return {
            enabled: !!noiseGateEnable?.checked,
            thresholdDb: aeClampNoiseGateThresholdDb(
                parseFloat(noiseGateThresholdSlider?.value ?? String(AE_NOISE_GATE_THRESHOLD_DEFAULT))
            ),
            releaseMs: aeClampNoiseGateReleaseMs(
                parseFloat(noiseGateReleaseSlider?.value ?? String(AE_NOISE_GATE_RELEASE_DEFAULT))
            ),
            depthDb: aeClampNoiseGateDepthDb(
                parseFloat(noiseGateDepthSlider?.value ?? String(AE_NOISE_GATE_DEPTH_DEFAULT))
            ),
        };
    }

    /** Restore gate UI + engine from preset; older presets without `noiseGate` use defaults. */
    function applyPresetNoiseGate(preset) {
        const ng = preset && typeof preset.noiseGate === 'object' && preset.noiseGate !== null
            ? preset.noiseGate
            : null;
        const defaults = {
            enabled: false,
            thresholdDb: AE_NOISE_GATE_THRESHOLD_DEFAULT,
            releaseMs: AE_NOISE_GATE_RELEASE_DEFAULT,
            depthDb: AE_NOISE_GATE_DEPTH_DEFAULT,
        };
        const enabled = ng && typeof ng.enabled === 'boolean' ? ng.enabled : defaults.enabled;
        let thresholdDb = defaults.thresholdDb;
        let releaseMs = defaults.releaseMs;
        let depthDb = defaults.depthDb;
        if (ng) {
            const t = Number(ng.thresholdDb);
            if (Number.isFinite(t)) thresholdDb = aeClampNoiseGateThresholdDb(t);
            const r = Number(ng.releaseMs);
            if (Number.isFinite(r)) releaseMs = aeClampNoiseGateReleaseMs(r);
            const d = Number(ng.depthDb);
            if (Number.isFinite(d)) depthDb = aeClampNoiseGateDepthDb(d);
        }
        if (noiseGateEnable) noiseGateEnable.checked = enabled;
        if (noiseGateThresholdSlider) noiseGateThresholdSlider.value = String(thresholdDb);
        if (noiseGateReleaseSlider) noiseGateReleaseSlider.value = String(releaseMs);
        if (noiseGateDepthSlider) noiseGateDepthSlider.value = String(depthDb);
        aeSyncNoiseGateThresholdLabel();
        aeSyncNoiseGateReleaseLabel();
        aeSyncNoiseGateDepthLabel();
        aeSyncNoiseGatePanelVisibility();
        aeApplyNoiseGateToEngine();
    }

    function aeInitNoiseGateUi() {
        if (noiseGateEnable) noiseGateEnable.checked = false;
        // Slider defaults match screen.html; no global localStorage — restored per preset via applyPresetNoiseGate.
        if (noiseGateThresholdSlider) noiseGateThresholdSlider.value = String(AE_NOISE_GATE_THRESHOLD_DEFAULT);
        if (noiseGateReleaseSlider) noiseGateReleaseSlider.value = String(AE_NOISE_GATE_RELEASE_DEFAULT);
        if (noiseGateDepthSlider) noiseGateDepthSlider.value = String(AE_NOISE_GATE_DEPTH_DEFAULT);
        aeSyncNoiseGateThresholdLabel();
        aeSyncNoiseGateReleaseLabel();
        aeSyncNoiseGateDepthLabel();
        aeSyncNoiseGatePanelVisibility();
    }

    function aeApplyNoiseGateToEngine() {
        const bridge = window.slopsmithDesktop?.audio;
        if (!bridge || typeof bridge.setNoiseGate !== 'function') {
            if (bridge && !window._aeNoiseGateBridgeWarned) {
                window._aeNoiseGateBridgeWarned = true;
                console.warn('[audio-engine] audio.setNoiseGate is not available — wire the native engine to enable processing.');
            }
            return;
        }
        const thresholdDb = aeClampNoiseGateThresholdDb(
            parseFloat(noiseGateThresholdSlider?.value ?? String(AE_NOISE_GATE_THRESHOLD_DEFAULT))
        );
        const releaseMs = aeClampNoiseGateReleaseMs(
            parseFloat(noiseGateReleaseSlider?.value ?? String(AE_NOISE_GATE_RELEASE_DEFAULT))
        );
        const depthDb = aeClampNoiseGateDepthDb(
            parseFloat(noiseGateDepthSlider?.value ?? String(AE_NOISE_GATE_DEPTH_DEFAULT))
        );
        bridge.setNoiseGate({
            enabled: !!noiseGateEnable?.checked,
            thresholdDb,
            releaseMs,
            depthDb,
        });
    }

    // ── Init ──────────────────────────────────────────────────────────────────
    async function init() {
        const available = await api.isAvailable();
        if (!available) {
            statusText.textContent = 'Audio engine not loaded (build with npm run build:audio)';
            return;
        }

        statusDot.className = 'w-3 h-3 rounded-full bg-yellow-500';
        statusText.textContent = 'Audio engine ready — not started';
        toggleBtn.disabled = false;

        await loadDeviceTypes();
        await refreshChain();
        api.loadPluginList();
        aeInitNoiseGateUi();
        setupEvents();
        startMetering();

        // Restore saved device settings and auto-start
        const saved = loadDeviceSettings();
        if (saved) {
            if (saved.type && deviceTypeSelect.querySelector(`option[value="${saved.type}"]`)) {
                deviceTypeSelect.value = saved.type;
                const typeInfo = currentDeviceTypes.find(t => t.name === saved.type);
                if (typeInfo) updateDeviceDropdowns(typeInfo);
            }
            if (saved.input) inputDeviceSelect.value = saved.input;
            if (saved.output) outputDeviceSelect.value = saved.output;
            if (saved.sampleRate) sampleRateSelect.value = saved.sampleRate;
            if (saved.bufferSize) bufferSizeSelect.value = saved.bufferSize;
            if (saved.inputChannel) inputChannelSelect.value = saved.inputChannel;
            if (saved.monitorMute !== undefined) monitorMuteCheckbox.checked = saved.monitorMute;

            // Auto-apply and start
            await api.setDeviceType(saved.type);
            const ok = await api.setDevice(
                saved.input || '', saved.output || '',
                parseFloat(saved.sampleRate || '48000'),
                parseInt(saved.bufferSize || '256')
            );
            if (ok) {
                if (saved.inputChannel) api.setInputChannel(parseInt(saved.inputChannel));
                if (saved.monitorMute !== undefined) api.setMonitorMute(saved.monitorMute);
                await api.startAudio();
                audioRunning = true;
                toggleBtn.textContent = 'Stop';
                statusDot.className = 'w-3 h-3 rounded-full bg-emerald-500';
                statusText.textContent = 'Audio running';
                aeApplyNoiseGateToEngine();
            }
        }

        // Try the default preset first; only restore the saved chain if no default preset is
        // configured or the preset load fails (corrupted blob, missing VST, etc.). This avoids
        // redundant native load/unload when the preset immediately replaces the chain, while
        // ensuring a valid chain is always available as a fallback.
        let _defaultLoaded = false;
        try {
            _defaultLoaded = await loadDefaultPreset('app-init');
        } catch (e) {
            console.error('[audio-engine] Default preset load threw at init; falling back to saved chain:', e);
        }
        if (!_defaultLoaded) {
            let savedChain;
            try {
                savedChain = JSON.parse(localStorage.getItem('slopsmith-signal-chain') || '[]');
                if (!Array.isArray(savedChain)) savedChain = [];
            } catch (e) {
                console.warn('[audio-engine] Corrupted slopsmith-signal-chain; starting empty:', e);
                savedChain = [];
            }
            for (const item of savedChain) {
                try {
                    if (item.type === 'VST' && item.path) {
                        await api.loadVST(item.path);
                    } else if (item.type === 'NAM' && item.path) {
                        await api.loadNAMModel(item.path);
                    } else if (item.type === 'IR' && item.path) {
                        await api.loadIR(item.path);
                    }
                } catch (e) {
                    console.error('[audio-engine] Failed to restore chain item:', item, e);
                }
            }
            if (savedChain.length > 0) await refreshChain();
        }

        aeApplyNoiseGateToEngine();
    }

    function saveChainStateFromChain(chain) {
        const typeMap = { 0: 'VST', 1: 'NAM', 2: 'IR' };
        const items = chain.filter(s => s.type === 0 || s.type === 1 || s.type === 2).map(s => ({
            type: typeMap[s.type] || 'VST',
            path: s.path || '',
            name: s.name || '',
        }));
        try { localStorage.setItem('slopsmith-signal-chain', JSON.stringify(items)); } catch (_) {}
    }

    function saveChainState() {
        api.getChainState().then(saveChainStateFromChain).catch(() => {});
    }

    function captureCurrentGainLevels() {
        const inDb = parseFloat(inputGainSlider?.value ?? '0');
        const outDb = parseFloat(outputGainSlider?.value ?? '0');
        return {
            inputGain: dbToLinearGain(inDb),
            outputGain: dbToLinearGain(outDb),
        };
    }

    function applyPresetGainLevels(preset) {
        const inputLin = Number.isFinite(Number(preset?.inputGain)) ? Number(preset.inputGain) : 1;
        const outputLin = Number.isFinite(Number(preset?.outputGain)) ? Number(preset.outputGain) : 1;

        const inDb = linearGainToDb(inputLin);
        const outDb = linearGainToDb(outputLin);

        // Round to 0.1 dB before updating the UI and engine so they always agree.
        const inDbR = parseFloat(inDb.toFixed(1));
        const outDbR = parseFloat(outDb.toFixed(1));
        if (inputGainSlider) inputGainSlider.value = inDbR;
        if (outputGainSlider) outputGainSlider.value = outDbR;
        if (inputGainLabel) inputGainLabel.textContent = formatGainDbLabel(inDbR);
        if (outputGainLabel) outputGainLabel.textContent = formatGainDbLabel(outDbR);

        // Round-trip through the clamped dB value so the engine sees the same gain
        // the slider shows — prevents out-of-range preset values from bypassing the
        // [-60, +12] dB clamp applied by linearGainToDb/dbToLinearGain.
        // Output gain routes to 'chain' (guitar-only, applied before the
        // backing track is mixed) so a tone-preset switch changes the amp
        // level without moving the song volume.
        api.setGain('input', dbToLinearGain(inDbR));
        api.setGain('chain', dbToLinearGain(outDbR));
    }

    // ── Device Types ──────────────────────────────────────────────────────────
    async function loadDeviceTypes() {
        currentDeviceTypes = await api.getDeviceTypes();
        deviceTypeSelect.innerHTML = '';

        for (const type of currentDeviceTypes) {
            const opt = document.createElement('option');
            opt.value = type.name;
            opt.textContent = type.name;
            deviceTypeSelect.appendChild(opt);
        }

        if (currentDeviceTypes.length > 0) {
            updateDeviceDropdowns(currentDeviceTypes[0]);
        }

        // Load current device info
        const current = await api.getCurrentDevice();
        if (current && current.type) {
            deviceTypeSelect.value = current.type;
            const typeInfo = currentDeviceTypes.find(t => t.name === current.type);
            if (typeInfo) updateDeviceDropdowns(typeInfo);
            if (current.input) inputDeviceSelect.value = current.input;
            if (current.output) outputDeviceSelect.value = current.output;
        }
    }

    function updateDeviceDropdowns(typeInfo) {
        inputDeviceSelect.innerHTML = '<option value="">Default</option>';
        for (const name of typeInfo.inputs) {
            const opt = document.createElement('option');
            opt.value = name;
            opt.textContent = name;
            inputDeviceSelect.appendChild(opt);
        }

        outputDeviceSelect.innerHTML = '<option value="">Default</option>';
        for (const name of typeInfo.outputs) {
            const opt = document.createElement('option');
            opt.value = name;
            opt.textContent = name;
            outputDeviceSelect.appendChild(opt);
        }
    }

    // ── Signal Chain ──────────────────────────────────────────────────────────
    async function refreshChain() {
        const container = chainContainer || $('ae-chain');
        if (!container) return null;
        const chain = await api.getChainState();
        container.innerHTML = '';

        if (chain.length === 0) {
            container.innerHTML = '<div class="text-sm text-slate-500 italic">No processors loaded — add a VST, NAM model, or cabinet IR</div>';
            return chain;
        }

        const typeNames = { 0: 'VST', 1: 'NAM', 2: 'IR' };
        const typeColors = { 0: 'purple', 1: 'orange', 2: 'cyan' };

        for (const slot of chain) {
            const color = typeColors[slot.type] || 'slate';
            const div = document.createElement('div');
            div.className = `flex items-center gap-3 p-3 rounded bg-slate-800/50 border border-${color}-500/30`;
            div.innerHTML = `
                <span class="text-xs font-medium px-2 py-0.5 rounded bg-${color}-500/20 text-${color}-400">
                    ${typeNames[slot.type] || '?'}
                </span>
                <span class="flex-1 text-sm ${slot.bypassed ? 'line-through text-slate-500' : 'text-slate-200'}">${slot.name}</span>
                ${slot.hasEditor ? `<button class="text-xs px-2 py-1 rounded bg-blue-600/50 hover:bg-blue-500"
                        onclick="_aeOpenEditor(${slot.id})">Edit</button>` : ''}
                <button class="text-xs px-2 py-1 rounded ${slot.bypassed ? 'bg-yellow-600' : 'bg-slate-600'} hover:opacity-80"
                        onclick="_aeToggleBypass(${slot.id}, ${!slot.bypassed})">
                    ${slot.bypassed ? 'Enable' : 'Bypass'}
                </button>
                <button class="text-xs px-2 py-1 rounded bg-red-600/50 hover:bg-red-500"
                        onclick="_aeRemoveSlot(${slot.id})">Remove</button>
            `;
            container.appendChild(div);
        }
        return chain;
    }

    // Global functions for inline onclick handlers
    window._aeToggleBypass = async (slotId, bypassed) => {
        await api.setBypass(slotId, bypassed);
        await refreshChain();
    };

    window._aeRemoveSlot = async (slotId) => {
        await api.closePluginEditor(slotId);
        await api.removeProcessor(slotId);
        await refreshChain();
    };

    window._aeOpenEditor = async (slotId) => {
        await api.openPluginEditor(slotId);
    };

    // ── VST Browser ───────────────────────────────────────────────────────────
    function renderVSTList(filter = '') {
        vstList.innerHTML = '';
        const filtered = filter
            ? knownPlugins.filter(p => (p.name + p.manufacturer + p.category).toLowerCase().includes(filter.toLowerCase()))
            : knownPlugins;

        if (filtered.length === 0) {
            vstList.innerHTML = '<div class="text-sm text-slate-500 italic">No plugins found</div>';
            return;
        }

        for (const plugin of filtered) {
            const div = document.createElement('div');
            div.className = 'flex items-center gap-3 p-2 rounded hover:bg-slate-700/50 cursor-pointer';
            div.innerHTML = `
                <div class="flex-1">
                    <div class="text-sm text-slate-200">${plugin.name}</div>
                    <div class="text-xs text-slate-400">${plugin.manufacturer} · ${plugin.format}</div>
                </div>
            `;
            div.addEventListener('click', async () => {
                const slotId = await api.loadVST(plugin.path);
                if (slotId >= 0) {
                    vstBrowser.classList.add('hidden');
                    await refreshChain();
                }
            });
            vstList.appendChild(div);
        }
    }

    // ── Metering ──────────────────────────────────────────────────────────────
    let meterPollInterval = null;

    function startMetering() {
        // Use setInterval at ~30fps instead of rAF to avoid overwhelming IPC
        if (meterPollInterval) clearInterval(meterPollInterval);

        meterPollInterval = setInterval(async () => {
            if (!audioRunning) {
                meterInput.style.width = '0%';
                meterOutput.style.width = '0%';
                return;
            }

            try {
                const levels = await api.getLevels();
                // Convert linear amplitude to dB-like scale for better visibility
                // Maps 0.001 (-60dB) to 0%, 1.0 (0dB) to 100%
                const toMeterPct = (v) => Math.max(0, Math.min(100, (1 + Math.log10(Math.max(v, 0.001)) / 3) * 100));
                const inPct = toMeterPct(levels.inputLevel);
                const outPct = toMeterPct(levels.outputLevel);
                meterInput.style.width = inPct + '%';
                meterOutput.style.width = outPct + '%';

                // Clipping indicator
                meterInput.className = levels.inputLevel > 0.95
                    ? 'h-full bg-red-500 transition-all duration-75'
                    : 'h-full bg-emerald-500 transition-all duration-75';

                // Pitch detection
                const pitch = await api.getPitchDetection();
                if (pitch.midiNote >= 0) {
                    pitchNote.textContent = pitch.noteName;
                    pitchFreq.textContent = pitch.frequency.toFixed(1) + ' Hz';
                    const pos = 50 + (pitch.cents / 50) * 50;
                    pitchCentsBar.style.left = Math.max(0, Math.min(100, pos)) + '%';
                    pitchCentsBar.className = Math.abs(pitch.cents) < 10
                        ? 'absolute top-1 bottom-1 w-2 bg-emerald-400 rounded transition-all duration-75'
                        : 'absolute top-1 bottom-1 w-2 bg-yellow-400 rounded transition-all duration-75';
                } else {
                    pitchNote.textContent = '--';
                    pitchFreq.textContent = '-- Hz';
                }
            } catch (e) { /* ignore polling errors */ }
        }, 33); // ~30fps

        // Latency: poll less frequently
        setInterval(async () => {
            if (!audioRunning) return;
            try {
                const device = await api.getCurrentDevice();
                if (device?.latencyMs) latencyEl.textContent = device.latencyMs.toFixed(1) + 'ms';
            } catch (e) { /* ignore */ }
        }, 1000);
    }

    // ── Events ────────────────────────────────────────────────────────────────
    function setupEvents() {
        // Start/Stop audio
        toggleBtn.addEventListener('click', async () => {
            if (audioRunning) {
                await api.stopAudio();
                audioRunning = false;
                toggleBtn.textContent = 'Start';
                statusDot.className = 'w-3 h-3 rounded-full bg-yellow-500';
                statusText.textContent = 'Audio stopped';
            } else {
                await api.startAudio();
                audioRunning = true;
                toggleBtn.textContent = 'Stop';
                statusDot.className = 'w-3 h-3 rounded-full bg-emerald-500';
                statusText.textContent = 'Audio running';
                aeApplyNoiseGateToEngine();
            }
        });

        // Device type change
        deviceTypeSelect.addEventListener('change', () => {
            const typeInfo = currentDeviceTypes.find(t => t.name === deviceTypeSelect.value);
            if (typeInfo) updateDeviceDropdowns(typeInfo);
        });

        // Apply device settings and start audio
        applyDeviceBtn.addEventListener('click', async () => {
            statusText.textContent = 'Configuring device...';
            // Stop audio first to release the device before reconfiguring
            if (audioRunning) {
                await api.stopAudio();
                audioRunning = false;
            }
            const typeName = deviceTypeSelect.value;
            await api.setDeviceType(typeName);
            const ok = await api.setDevice(
                inputDeviceSelect.value,
                outputDeviceSelect.value,
                parseFloat(sampleRateSelect.value),
                parseInt(bufferSizeSelect.value)
            );
            if (ok) {
                await api.startAudio();
                audioRunning = true;
                toggleBtn.textContent = 'Stop';
                statusDot.className = 'w-3 h-3 rounded-full bg-emerald-500';
                statusText.textContent = 'Audio running';
                aeApplyNoiseGateToEngine();
                saveDeviceSettings();
            } else {
                statusText.textContent = 'Failed to configure device';
                statusDot.className = 'w-3 h-3 rounded-full bg-red-500';
            }
        });

        // Input channel
        inputChannelSelect.addEventListener('change', () => {
            api.setInputChannel(parseInt(inputChannelSelect.value));
        });

        // Monitor mute
        monitorMuteCheckbox.addEventListener('change', () => {
            api.setMonitorMute(monitorMuteCheckbox.checked);
        });

        // Gain sliders (UI dB → linear amplitude for engine)
        inputGainSlider.addEventListener('input', () => {
            const db = parseFloat(inputGainSlider.value);
            api.setGain('input', dbToLinearGain(db));
            inputGainLabel.textContent = formatGainDbLabel(db);
        });

        outputGainSlider.addEventListener('input', () => {
            const db = parseFloat(outputGainSlider.value);
            // 'chain' = guitar-only amp output (see applyPresetGainLevels).
            api.setGain('chain', dbToLinearGain(db));
            outputGainLabel.textContent = formatGainDbLabel(db);
        });

        if (noiseGateEnable) {
            noiseGateEnable.addEventListener('change', () => {
                // aeSyncNoiseGatePanelVisibility() internally no-ops when noiseGateThresholdWrap is missing.
                aeSyncNoiseGatePanelVisibility();
                aeApplyNoiseGateToEngine();
            });
        }
        if (noiseGateThresholdSlider) {
            noiseGateThresholdSlider.addEventListener('input', () => {
                aeSyncNoiseGateThresholdLabel();
                aeApplyNoiseGateToEngine();
            });
        }
        if (noiseGateReleaseSlider) {
            noiseGateReleaseSlider.addEventListener('input', () => {
                aeSyncNoiseGateReleaseLabel();
                aeApplyNoiseGateToEngine();
            });
        }
        if (noiseGateDepthSlider) {
            noiseGateDepthSlider.addEventListener('input', () => {
                aeSyncNoiseGateDepthLabel();
                aeApplyNoiseGateToEngine();
            });
        }

        // Add VST
        addVstBtn.addEventListener('click', () => {
            vstBrowser.classList.toggle('hidden');
            if (!vstBrowser.classList.contains('hidden') && knownPlugins.length > 0) {
                renderVSTList();
            }
        });

        // Add NAM model
        addNamBtn.addEventListener('click', async () => {
            const filePath = await window.slopsmithDesktop.pickFile([
                { name: 'NAM Models', extensions: ['nam'] }
            ]);
            if (filePath) {
                const slotId = await api.loadNAMModel(filePath);
                if (slotId >= 0) { await refreshChain(); saveChainState(); }
            }
        });

        // Add IR
        addIrBtn.addEventListener('click', async () => {
            console.error('[audio-engine] IR button clicked, opening picker...');
            const filePath = await window.slopsmithDesktop.pickFile([
                { name: 'Impulse Responses', extensions: ['wav', 'aif', 'ir'] },
                { name: 'All Files', extensions: ['*'] }
            ]);
            console.error('[audio-engine] IR picker returned:', filePath);
            if (filePath) {
                const slotId = await api.loadIR(filePath);
                console.error('[audio-engine] loadIR returned slotId:', slotId);
                if (slotId >= 0) { await refreshChain(); saveChainState(); }
            }
        });

        // Clear chain
        clearChainBtn.addEventListener('click', async () => {
            await api.clearChain();
            await refreshChain();
            saveChainState();
        });

        // Scan VSTs
        scanVstsBtn.addEventListener('click', async () => {
            scanVstsBtn.disabled = true;
            scanVstsBtn.textContent = 'Scanning...';
            try {
                knownPlugins = await api.scanPlugins();
                await api.savePluginList();
                renderVSTList();
                scanVstsBtn.textContent = `Scan (${knownPlugins.length} found)`;
            } catch (e) {
                scanVstsBtn.textContent = 'Scan Failed';
            }
            scanVstsBtn.disabled = false;
        });

        // VST search
        vstSearch.addEventListener('input', () => {
            renderVSTList(vstSearch.value);
        });

        // Save preset with name
        savePresetBtn.addEventListener('click', async () => {
            // Show inline name input
            const existing = $('ae-preset-name-input');
            if (existing) { existing.focus(); return; }
            const wrapper = document.createElement('div');
            wrapper.id = 'ae-preset-name-input';
            wrapper.className = 'flex gap-2 mt-2';
            wrapper.innerHTML = `
                <input type="text" placeholder="Preset name..." class="flex-1 bg-slate-700 border border-slate-600 rounded px-3 py-1.5 text-sm text-slate-200" autofocus>
                <button class="px-3 py-1.5 rounded bg-emerald-600 hover:bg-emerald-500 text-sm">Save</button>
                <button class="px-3 py-1.5 rounded bg-slate-600 hover:bg-slate-500 text-sm">Cancel</button>
            `;
            savePresetBtn.parentElement.after(wrapper);
            const input = wrapper.querySelector('input');
            const [saveBtn, cancelBtn] = wrapper.querySelectorAll('button');
            input.focus();

            const doSave = async () => {
                const name = input.value.trim();
                if (!name) return;
                const nativePreset = await api.savePreset();
                if (!nativePreset) return;
                const chain = await api.getChainState();
                const items = chain.map(s => ({
                    type: s.type === 0 ? 'VST' : s.type === 1 ? 'NAM' : 'IR',
                    path: s.path || '',
                    name: s.name || '',
                }));
                const gains = captureCurrentGainLevels();
                const noiseGate = captureCurrentNoiseGateState();
                const presets = getPresets();
                presets[name] = { nativePreset, items, ...gains, noiseGate, created: Date.now() };
                localStorage.setItem('slopsmith-chain-presets', JSON.stringify(presets));
                wrapper.remove();
                renderPresetList();
                renderToneAutomationTargets();
                // Refresh floating panel if open
                if (document.getElementById('ae-tone-panel-float')) {
                    closeTonePanel();
                    void toggleTonePanel();
                }
            };

            saveBtn.addEventListener('click', doSave);
            input.addEventListener('keydown', (e) => { if (e.key === 'Enter') doSave(); });
            cancelBtn.addEventListener('click', () => wrapper.remove());
        });

        // Sync offset (in settings panel — innerHTML doesn't run scripts, so we bind here)
        const syncSlider = document.getElementById('ae-sync-offset');
        const syncLabel = document.getElementById('ae-sync-offset-label');
        if (syncSlider && syncLabel) {
            const saved = localStorage.getItem('slopsmith-sync-offset');
            if (saved !== null) {
                syncSlider.value = parseFloat(saved);
                window._slopsmithSyncOffset = parseFloat(saved);
                syncLabel.textContent = Math.round(parseFloat(saved) * 1000) + 'ms';
            }
            syncSlider.addEventListener('input', () => {
                const val = parseFloat(syncSlider.value);
                window._slopsmithSyncOffset = val;
                syncLabel.textContent = Math.round(val * 1000) + 'ms';
                localStorage.setItem('slopsmith-sync-offset', String(val));
            });
        }

        setupAudioQualityControls();
        setupToneAutomationSettingsEvents();
    }

    // ── Audio Quality (soundfont) ─────────────────────────────────────────────
    function setupAudioQualityControls() {
        const api = window.slopsmithDesktop?.soundfont;
        const defaultRadio = document.getElementById('ae-sf-default');
        const highRadio = document.getElementById('ae-sf-high');
        const highStatus = document.getElementById('ae-sf-high-status');
        const downloadBtn = document.getElementById('ae-sf-download');
        const cancelBtn = document.getElementById('ae-sf-cancel');
        const progress = document.getElementById('ae-sf-progress');
        const progressLabel = document.getElementById('ae-sf-progress-label');
        const msg = document.getElementById('ae-sf-message');

        if (!api || !defaultRadio || !highRadio || !downloadBtn) return;

        function fmtMB(bytes) {
            return (bytes / (1024 * 1024)).toFixed(1);
        }

        function showMessage(text, kind) {
            msg.textContent = text;
            msg.classList.remove('hidden', 'text-slate-400', 'text-green-400', 'text-red-400');
            msg.classList.add(kind === 'error' ? 'text-red-400' : kind === 'success' ? 'text-green-400' : 'text-slate-400');
        }

        async function refresh() {
            const status = await api.getStatus();
            defaultRadio.checked = status.activeQuality === 'default';
            highRadio.checked = status.activeQuality === 'high';
            highRadio.disabled = !status.highDownloaded;

            if (status.highDownloaded) {
                highStatus.textContent = `Downloaded. ${status.activeQuality === 'high' ? 'Active.' : 'Select to activate.'}`;
                downloadBtn.textContent = 'Redownload';
            } else {
                highStatus.textContent = 'Not downloaded yet.';
                downloadBtn.textContent = `Download ${status.expectedSizeMB} MB`;
            }

            downloadBtn.disabled = status.downloadInProgress;
            cancelBtn.classList.toggle('hidden', !status.downloadInProgress);
            progress.classList.toggle('hidden', !status.downloadInProgress);
            progressLabel.classList.toggle('hidden', !status.downloadInProgress);
        }

        downloadBtn.addEventListener('click', async () => {
            downloadBtn.disabled = true;
            cancelBtn.classList.remove('hidden');
            progress.classList.remove('hidden');
            progressLabel.classList.remove('hidden');
            progress.value = 0;
            progressLabel.textContent = 'Starting download…';
            msg.classList.add('hidden');

            const result = await api.downloadHighQuality();
            cancelBtn.classList.add('hidden');
            if (result.success) {
                showMessage('Download complete. Select "High" to activate.', 'success');
            } else {
                progress.classList.add('hidden');
                progressLabel.classList.add('hidden');
                showMessage(result.message, 'error');
            }
            await refresh();
        });

        cancelBtn.addEventListener('click', async () => {
            await api.cancelDownload();
            showMessage('Download cancelled.', 'info');
            await refresh();
        });

        async function handleQualityChange(quality) {
            const result = await api.setQuality(quality);
            if (result.success) {
                showMessage(result.message, 'info');
            } else {
                showMessage(result.message, 'error');
                await refresh();
            }
        }

        defaultRadio.addEventListener('change', () => {
            if (defaultRadio.checked) handleQualityChange('default');
        });
        highRadio.addEventListener('change', () => {
            if (highRadio.checked) handleQualityChange('high');
        });

        api.onDownloadProgress(({ bytesDownloaded, totalBytes, percent }) => {
            progress.value = percent;
            const total = totalBytes > 0 ? `${fmtMB(bytesDownloaded)} / ${fmtMB(totalBytes)} MB` : `${fmtMB(bytesDownloaded)} MB`;
            progressLabel.textContent = `${total} (${percent.toFixed(0)}%)`;
        });

        refresh();
    }

    // ── Settings path pickers ──────────────────────────────────────────────────
    function setupPathPickers() {
        const pickers = [
            { btn: 'ae-pick-dlc', input: 'ae-dlc-path', key: 'dlcDir' },
            { btn: 'ae-pick-nam', input: 'ae-nam-path', key: 'namDir' },
            { btn: 'ae-pick-ir', input: 'ae-ir-path', key: 'irDir' },
        ];
        for (const { btn, input, key } of pickers) {
            const btnEl = $(btn);
            const inputEl = $(input);
            if (!btnEl || !inputEl) continue;

            // Load saved value
            const saved = localStorage.getItem('slopsmith-' + key);
            if (saved) inputEl.value = saved;

            btnEl.addEventListener('click', async () => {
                const dir = await window.slopsmithDesktop.pickDirectory();
                if (dir) {
                    inputEl.value = dir;
                    localStorage.setItem('slopsmith-' + key, dir);
                    // If it's the DLC dir, also update the server
                    if (key === 'dlcDir') {
                        await fetch('/api/settings', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ dlc_dir: dir }),
                        });
                    }
                }
            });
        }
    }
    setupPathPickers();

    // ── Preset Management ──────────────────────────────────────────────────────
    function getPresets() {
        try {
            const parsed = JSON.parse(localStorage.getItem('slopsmith-chain-presets') || '{}');
            if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
            return parsed;
        } catch (e) {
            return {};
        }
    }

    function getDefaultPresetName() {
        return localStorage.getItem('slopsmith-default-preset-name') || '';
    }

    function setDefaultPresetName(name) {
        if (!name) localStorage.removeItem('slopsmith-default-preset-name');
        else localStorage.setItem('slopsmith-default-preset-name', name);
    }

    /** Older saved presets may omit `items`; iterating undefined throws and can crash the embedded UI. */
    function getPresetItems(preset) {
        const items = preset?.items;
        return Array.isArray(items) ? items : [];
    }

    function markSongTransition(durationMs = 7000) {
        const until = Date.now() + Math.max(1000, Number(durationMs) || 7000);
        window._aeSongTransitionUntil = until;
        return until;
    }

    function isSongTransitioning() {
        return Date.now() < (window._aeSongTransitionUntil || 0);
    }

    window._aeMarkSongTransition = markSongTransition;

    /** Replace the entire native chain with a saved preset blob. Always clears first so
     *  the previous menu/player chain is fully torn down and sounds cannot stack.
     *  On loadPreset failure the previous chain is restored (best-effort). */
    async function replaceChainWithPresetBlob(preset, logCtx = '', { snapshot = true } = {}) {
        if (!preset?.nativePreset) return false;
        const tag = '[audio-engine] replaceChainWithPresetBlob' + (logCtx ? ` (${logCtx})` : '');

        // Snapshot via savePreset() before clearing so rollback restores full plugin
        // state (parameters, not just paths) if loadPreset fails.
        // Pass snapshot:false for automated/frequent callers (tone automation, preload)
        // to avoid the IPC overhead of serializing the full chain on every tone switch.
        let snapshotBlob = null;
        if (snapshot) {
            try { snapshotBlob = await api.savePreset(); } catch (_) { /* best-effort */ }
        }

        try {
            await api.clearChain();
            const result = await api.loadPreset(preset.nativePreset);
            // Some JUCE bridges return {success:false} or bare false instead of throwing.
            if (result === false || (result && result.success === false)) {
                console.error(tag + ': loadPreset failed:', result?.error || 'unknown error');
                await _restorePresetBlob(snapshotBlob, tag);
                if (!snapshotBlob) {
                    // No rollback available; chain is now empty — sync localStorage and UI to match.
                    try { localStorage.setItem('slopsmith-signal-chain', '[]'); } catch (_) {}
                    _renderEmptyChain();
                }
                return false;
            }
            applyPresetGainLevels(preset);
            applyPresetNoiseGate(preset);
            // Share the single getChainState() result between refreshChain and saveChainState
            // to avoid two back-to-back native bridge round-trips.
            const chain = await refreshChain();
            if (Array.isArray(chain)) saveChainStateFromChain(chain);
            else saveChainState();
            return true;
        } catch (e) {
            console.error(tag + ':', e);
            await _restorePresetBlob(snapshotBlob, tag);
            if (!snapshotBlob) {
                // No rollback available; chain is now empty — sync localStorage and UI to match.
                try { localStorage.setItem('slopsmith-signal-chain', '[]'); } catch (_) {}
                _renderEmptyChain();
            }
            return false;
        }
    }

    function _renderEmptyChain() {
        const container = chainContainer || $('ae-chain');
        if (container) container.innerHTML = '<div class="text-sm text-slate-500 italic">No processors loaded — add a VST, NAM model, or cabinet IR</div>';
    }

    async function _restorePresetBlob(snapshotBlob, tag) {
        if (!snapshotBlob) return;
        try {
            await api.clearChain();
            const result = await api.loadPreset(snapshotBlob);
            // Some JUCE bridges return {success:false} or bare false instead of throwing.
            if (result === false || (result && result.success === false)) {
                console.warn((tag || '[audio-engine]') + ' snapshot rollback loadPreset failed:', result?.error || 'unknown');
                try { localStorage.setItem('slopsmith-signal-chain', '[]'); } catch (_) {}
                _renderEmptyChain();
                return;
            }
            await refreshChain();
        } catch (e) {
            console.warn((tag || '[audio-engine]') + ' snapshot rollback failed:', e);
        }
    }

    async function loadDefaultPreset(reason = 'manual') {
        if ((reason === 'player-exit' || reason === 'song-stop') && isSongTransitioning()) {
            console.log('[audio-engine] Skipping default preset during song transition:', reason);
            return false;
        }
        const presets = getPresets();
        // Only load when the user has explicitly set a default preset via the Default button.
        // Saving a preset no longer auto-promotes it, so this is always intentional.
        const defaultName = getDefaultPresetName();
        if (!defaultName || !presets[defaultName]) return false;
        const preset = presets[defaultName];
        if (!(await replaceChainWithPresetBlob(preset, `default:${defaultName}`))) return false;
        console.log('[audio-engine] Loaded default preset:', defaultName, 'reason:', reason);
        if (reason === 'player-exit' || reason === 'song-stop') {
            window._toneMappingsDirty = true;
            window._toneSwitcher = null;
        }
        return true;
    }

    window._aeGetPresets = getPresets;
    window._aeApplyPresetGainLevels = applyPresetGainLevels;
    window._aeApplyPresetNoiseGate = applyPresetNoiseGate;
    window._aeLoadDefaultPreset = loadDefaultPreset;
    window._aeReplaceChainWithPresetBlob = replaceChainWithPresetBlob;

    /** Clears the native FX chain when a new song starts. Avoid calling getChainState right after
     *  clearChain — some JUCE bridges crash on that sequence; persist empty chain locally instead. */
    async function clearChainForNewSong() {
        if (!api?.clearChain) return;
        try {
            await api.clearChain();
        } catch (e) {
            console.warn('[audio-engine] clearChain (native):', e);
            return;
        }
        try {
            localStorage.setItem('slopsmith-signal-chain', '[]');
        } catch (e) {
            console.warn('[audio-engine] persist empty chain:', e);
        }
        // Do NOT call refreshChain() here — it calls api.getChainState() which can crash
        // some JUCE bridges immediately after clearChain. Render the empty state directly.
        const container = chainContainer || $('ae-chain');
        if (container) {
            container.innerHTML = '<div class="text-sm text-slate-500 italic">No processors loaded — add a VST, NAM model, or cabinet IR</div>';
        }
    }
    window._aeClearChainForNewSong = clearChainForNewSong;

    function renderPresetList() {
        const container = $('ae-preset-list');
        if (!container) return;
        const presets = getPresets();
        const names = Object.keys(presets);
        // Read-only — don't use ensureDefaultPresetName() here; that persists an auto-selected
        // default and would cause loadDefaultPreset to apply it silently on the next startup.
        const defaultPresetName = getDefaultPresetName();
        if (names.length === 0) {
            container.innerHTML = '<div class="text-xs text-slate-500 italic">No saved presets</div>';
            return;
        }
        container.innerHTML = '';
        for (const name of names) {
            const div = document.createElement('div');
            div.className = 'flex items-center gap-2 p-2 rounded bg-slate-800/50 text-sm';
            const eName = escHtml(name);
            div.innerHTML = `
                <span class="flex-1 text-slate-300">${eName}${name === defaultPresetName ? ' <span class="text-xs text-slate-500">(default)</span>' : ''}</span>
                <span class="text-xs text-slate-500">${getPresetItems(presets[name]).length} processors</span>
                <button class="text-xs px-2 py-1 rounded ${name === defaultPresetName ? 'bg-blue-700/60 text-slate-300 cursor-not-allowed' : 'bg-blue-600/50 hover:bg-blue-500'}" data-preset="${eName}" data-action="default" ${name === defaultPresetName ? 'disabled' : ''}>Default</button>
                <button class="text-xs px-2 py-1 rounded bg-emerald-600/50 hover:bg-emerald-500" data-preset="${eName}" data-action="load">Load</button>
                <button class="text-xs px-2 py-1 rounded bg-red-600/50 hover:bg-red-500" data-preset="${eName}" data-action="delete">Del</button>
            `;
            div.querySelector('[data-action="default"]').addEventListener('click', () => {
                setDefaultPresetName(name);
                renderPresetList();
            });
            div.querySelector('[data-action="load"]').addEventListener('click', async () => {
                const p = getPresets()[name];
                if (!p) return;
                if (await replaceChainWithPresetBlob(p, `settings-load:${name}`)) {
                    console.log('[audio-engine] Preset loaded:', name);
                }
            });
            div.querySelector('[data-action="delete"]').addEventListener('click', () => {
                const ps = getPresets();
                delete ps[name];
                localStorage.setItem('slopsmith-chain-presets', JSON.stringify(ps));
                const deletedWasDefault = getDefaultPresetName() === name;
                if (deletedWasDefault) {
                    // Clear rather than auto-promote — the default should only be set explicitly.
                    setDefaultPresetName('');
                }
                // Scrub any Tone Automation targets that reference the deleted preset so
                // automation doesn't silently resolve to a non-existent preset.
                const taCfg = readTaStore();
                let taDirty = false;
                for (const [cat, presetRef] of Object.entries(taCfg.targets)) {
                    if (presetRef === name) { delete taCfg.targets[cat]; taDirty = true; }
                }
                if (taDirty) writeTaStore(taCfg);
                renderPresetList();
                renderToneAutomationTargets();
            });
            container.appendChild(div);
        }
    }

    // ── Tone Switching ───────────────────────────────────────────────────────────
    let toneSwitcher = null;
    let autoSwitchEnabled = localStorage.getItem('slopsmith-tone-auto-switch') === 'true';
    const originalToneNamesCache = new Map();

    class ToneSwitcher {
        constructor() {
            this.toneSlotMap = {};      // { toneName: [slotId, ...] }
            this.tonePresetMap = {};    // { toneName: preset }
            this.tonePresetNameMap = {}; // { toneName: presetName } — parallel map for O(1) name lookup
            this.activeTone = null;
        }

        async preloadForSong(toneChanges, toneBase, mappings) {
            const changes = Array.isArray(toneChanges) ? toneChanges : [];
            const rawBase = String(toneBase || '').trim();
            const toneNames = new Set();
            if (rawBase) toneNames.add(rawBase);
            for (const tc of changes) {
                const n = String(tc?.name || '').trim();
                if (n) toneNames.add(n);
            }
            let effectiveBase = rawBase;
            if (!effectiveBase && changes.length > 0) {
                const sorted = [...changes]
                    .filter(tc => String(tc?.name || '').trim())
                    .sort((a, b) => getToneChangeTime(a) - getToneChangeTime(b));
                if (sorted.length) effectiveBase = String(sorted[0].name).trim();
            }
            if (!effectiveBase && toneNames.size > 0) {
                effectiveBase = toneNames.values().next().value;
            }

            const presets = getPresets();
            this.toneSlotMap = {};
            this.tonePresetMap = {};
            this.tonePresetNameMap = {};
            this.activeTone = null;

            // Clear chain first
            await api.clearChain();

            if (toneNames.size === 0) {
                console.warn('[tone-switcher] preloadForSong: no valid tone names; chain cleared only');
                // Sync localStorage to reflect the now-empty chain so a restart won't
                // restore stale plugin state that no longer matches the engine.
                try { localStorage.setItem('slopsmith-signal-chain', '[]'); } catch (_) {}
                // Do NOT call refreshChain() — it calls api.getChainState() which can crash some
                // JUCE bridges immediately after clearChain. Render empty state directly instead.
                const container = chainContainer || $('ae-chain');
                if (container) {
                    container.innerHTML = '<div class="text-sm text-slate-500 italic">No processors loaded — add a VST, NAM model, or cabinet IR</div>';
                }
                return;
            }

            for (const toneName of toneNames) {
                const presetName = resolveTonePresetName(mappings, toneName);
                if (!presetName || !presets[presetName]) continue;

                const preset = presets[presetName];
                // Load each item individually and track slot IDs
                const slotIds = [];
                for (const item of getPresetItems(preset)) {
                    let slotId = -1;
                    if (item.type === 'NAM' && item.path) {
                        slotId = await api.loadNAMModel(item.path);
                    } else if (item.type === 'IR' && item.path) {
                        slotId = await api.loadIR(item.path);
                    } else if (item.type === 'VST' && item.path) {
                        slotId = await api.loadVST(item.path);
                    }
                    if (slotId >= 0) slotIds.push(slotId);
                }
                this.toneSlotMap[toneName] = slotIds;
                this.tonePresetMap[toneName] = preset;
                this.tonePresetNameMap[toneName] = presetName;

                // Bypass everything except the initial tone
                if (toneName !== effectiveBase) {
                    const bypassChanges = slotIds.map(id => ({ slotId: id, bypassed: true }));
                    if (bypassChanges.length > 0) await api.setMultiBypass(bypassChanges);
                }
            }

            this.activeTone = effectiveBase;
            const initialPreset = this.tonePresetMap[effectiveBase];
            if (initialPreset) {
                applyPresetGainLevels(initialPreset);
                applyPresetNoiseGate(initialPreset);
            }
            await refreshChain();
            console.log('[tone-switcher] Preloaded tones:', Object.keys(this.toneSlotMap));
        }

        switchToTone(toneName) {
            if (toneName === this.activeTone) return;
            if (!this.toneSlotMap[toneName]) return;

            const changes = [];
            // Bypass old tone
            if (this.activeTone && this.toneSlotMap[this.activeTone]) {
                for (const id of this.toneSlotMap[this.activeTone])
                    changes.push({ slotId: id, bypassed: true });
            }
            // Unbypass new tone
            for (const id of this.toneSlotMap[toneName])
                changes.push({ slotId: id, bypassed: false });

            if (changes.length > 0) api.setMultiBypass(changes);
            this.activeTone = toneName;
            const newPreset = this.tonePresetMap[toneName];
            if (newPreset) {
                applyPresetGainLevels(newPreset);
                applyPresetNoiseGate(newPreset);
            }
            console.log('[tone-switcher] Switched to:', toneName);
        }

        async teardown() {
            this.toneSlotMap = {};
            this.tonePresetMap = {};
            this.tonePresetNameMap = {};
            this.activeTone = null;
        }
    }

    function readToneMappingsStore() {
        let raw = {};
        try {
            raw = JSON.parse(localStorage.getItem('slopsmith-tone-mappings') || '{}') || {};
        } catch (e) {
            raw = {};
        }
        if (typeof raw !== 'object' || Array.isArray(raw)) raw = {};
        if (!raw.global || typeof raw.global !== 'object' || Array.isArray(raw.global)) raw.global = {};
        if (!raw.songs || typeof raw.songs !== 'object' || Array.isArray(raw.songs)) raw.songs = {};
        if (!raw.midiPC || typeof raw.midiPC !== 'object' || Array.isArray(raw.midiPC)) raw.midiPC = {};
        return raw;
    }

    function getToneMappings(songKey) {
        const all = readToneMappingsStore();
        const songMappings = songKey ? (all.songs[songKey] || {}) : {};
        return { ...all.global, ...songMappings };
    }

    function saveToneMappings(songKey, mappings) {
        const all = readToneMappingsStore();
        if (songKey) {
            all.songs[songKey] = mappings || {};
        } else {
            all.global = mappings || {};
        }
        localStorage.setItem('slopsmith-tone-mappings', JSON.stringify(all));
    }

    function getMidiPCConfig(songKey) {
        const all = readToneMappingsStore();
        return all.midiPC?.[songKey] || null;
    }

    function saveMidiPCConfig(songKey, config) {
        const all = readToneMappingsStore();
        if (config == null) delete all.midiPC[songKey];
        else all.midiPC[songKey] = config;
        localStorage.setItem('slopsmith-tone-mappings', JSON.stringify(all));
    }

    function normalizeSongKey(raw) {
        return String(raw || '')
            .replace(/\\/g, '/')
            .trim();
    }

    function getCurrentSongKey() {
        const current = normalizeSongKey(window._currentSongFile);
        if (current) {
            window._slopsmithSongKey = current;
            return current;
        }
        return normalizeSongKey(window._slopsmithSongKey || document.title || '');
    }

    function getToneChangeTime(tc) {
        const t = tc?.t ?? tc?.time ?? tc?.timestamp ?? tc?.at;
        return Number.isFinite(t) ? t : Infinity;
    }

    function _toneMatchKey(value) {
        return String(value || '').toLowerCase().replace(/[^a-z0-9]/g, '');
    }

    function findMappingForTone(mappings, toneName) {
        if (!mappings || typeof mappings !== 'object') return null;
        if (mappings[toneName]) return mappings[toneName];
        const target = _toneMatchKey(toneName);
        if (!target) return null;
        for (const key of Object.keys(mappings)) {
            if (key === '$default') continue;
            const k = _toneMatchKey(key);
            if (!k) continue;
            if (k === target || k.includes(target) || target.includes(k)) {
                return mappings[key];
            }
        }
        return null;
    }

    /** Chain preset name for a tone: explicit/fuzzy mapping → tone-mapping `$default` → app default preset (-- none --). */
    function resolveTonePresetName(mappings, toneName) {
        if (!mappings || typeof mappings !== 'object') mappings = {};
        const mapped = findMappingForTone(mappings, toneName);
        if (mapped) return mapped;
        if (mappings['$default']) return mappings['$default'];
        // Use getDefaultPresetName (non-mutating) — ensureDefaultPresetName writes to
        // localStorage and would silently create an implicit default just because tone
        // mappings were evaluated during playback, then apply it on the next startup.
        return getDefaultPresetName() || null;
    }

    window._aeFindMappingForTone = findMappingForTone;
    window._aeResolveTonePresetName = resolveTonePresetName;
    window._aeGetOriginalToneNamesForCurrentArrangement = getOriginalToneNamesForCurrentArrangement;
    window._aeNormalizeSongKey = normalizeSongKey;
    window._aeGetCurrentSongKey = getCurrentSongKey;
    window._aeGetToneChangeTime = getToneChangeTime;

    async function getOriginalToneNamesForCurrentArrangement(songKey) {
        const key = normalizeSongKey(songKey);
        if (!key) return [];
        try {
            const resp = await fetch(`/api/plugins/midi_amp/song-tones/${encodeURIComponent(key)}`);
            if (!resp.ok) return [];
            const data = await resp.json();
            const tones = Array.isArray(data?.tones) ? data.tones : [];
            const arr = String(window.slopsmith?.currentSong?.arrangement || '').trim().toLowerCase();
            const filtered = arr
                ? tones.filter(t => String(t?.arrangement || '').trim().toLowerCase() === arr)
                : tones;
            return Array.from(new Set(filtered.map(t => (t?.name || t?.key || '').trim()).filter(Boolean)));
        } catch (e) {
            return [];
        }
    }

    function getCurrentArrangementName() {
        return String(window.slopsmith?.currentSong?.arrangement || '').trim();
    }

    async function getOriginalToneNames(songKey, arrangementName = '') {
        const key = normalizeSongKey(songKey);
        const arr = String(arrangementName || '').trim().toLowerCase();
        if (!key) return [];
        const cacheKey = `${key}::${arr}::v2`;
        if (originalToneNamesCache.has(cacheKey)) return originalToneNamesCache.get(cacheKey);
        try {
            const resp = await fetch(`/api/plugins/midi_amp/song-tones/${encodeURIComponent(key)}`);
            if (!resp.ok) return [];
            const data = await resp.json();
            const tones = Array.isArray(data?.tones) ? data.tones : [];
            const filtered = arr
                ? tones.filter(t => String(t?.arrangement || '').trim().toLowerCase() === arr)
                : tones;
            const names = filtered
                .map(t => (t?.name || t?.key || '').trim())
                .filter(Boolean);
            // When an arrangement is set, never mix in tones from other tracks (e.g. Bass vs Rhythm).
            const finalNames = arr
                ? names
                : (names.length > 0
                    ? names
                    : tones.map(t => (t?.name || t?.key || '').trim()).filter(Boolean));
            const deduped = Array.from(new Set(finalNames));
            if (originalToneNamesCache.size >= 200) {
                originalToneNamesCache.delete(originalToneNamesCache.keys().next().value);
            }
            originalToneNamesCache.set(cacheKey, deduped);
            return deduped;
        } catch (e) {
            return [];
        }
    }

    async function normalizeTimelineToneData(songKey, toneChanges, toneBase, arrangementName = '') {
        const originalNames = await getOriginalToneNames(songKey, arrangementName);
        if (originalNames.length === 0) {
            return {
                toneChanges: Array.isArray(toneChanges) ? toneChanges : [],
                toneBase: toneBase || '',
                originalNames,
            };
        }
        const toneKey = (v) => String(v || '').toLowerCase().replace(/[^a-z0-9]/g, '');
        const keys = originalNames.map(toneKey).filter(Boolean);
        const matchesOriginal = (name) => {
            const k = toneKey(name);
            if (!k) return false;
            return keys.some(ok => ok === k || ok.includes(k) || k.includes(ok));
        };
        const baseOk = !!toneBase && matchesOriginal(toneBase);
        const changes = Array.isArray(toneChanges) ? toneChanges : [];
        const hasMatchingChange = changes.some(tc => {
            const name = String(tc?.name || '');
            return matchesOriginal(name);
        });
        if (baseOk || hasMatchingChange) {
            return { toneChanges: changes, toneBase: toneBase || '', originalNames };
        }
        // No overlap with active arrangement tones: treat timeline data as stale.
        return { toneChanges: [], toneBase: '', originalNames };
    }

    function escHtml(value) {
        return String(value ?? '')
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    function startToneMonitor() {
        // If IIFE2's startToneAutoSwitch is already running its own 50ms polling loop,
        // don't start a parallel interval — both would call switchToTone at 50ms cadence.
        if (window._toneAutoSwitchActive) return;
        if (hookState.toneMonitorInterval) clearInterval(hookState.toneMonitorInterval);
        hookState.toneMonitorInterval = setInterval(() => {
            if (!toneSwitcher || !autoSwitchEnabled) return;
            const hw = window.highway || window._slopsmithHighway;
            if (!hw || !hw.getTime) return;
            const t = hw.getTime();
            const changes = hw.getToneChanges ? hw.getToneChanges() : [];
            const base = hw.getToneBase ? hw.getToneBase() : '';

            let activeTone = String(base || '').trim();
            for (const tc of changes) {
                if (getToneChangeTime(tc) <= t) {
                    const n = String(tc?.name || '').trim();
                    if (n) activeTone = n;
                } else break;
            }
            if (activeTone) toneSwitcher.switchToTone(activeTone);
        }, 50);
    }

    function getActiveToneAtTime(timeSec, toneChanges, toneBase) {
        let activeTone = String(toneBase || '').trim();
        for (const tc of toneChanges) {
            if (getToneChangeTime(tc) <= timeSec) {
                const n = String(tc?.name || '').trim();
                if (n) activeTone = n;
            } else break;
        }
        return activeTone;
    }

    let _applyToneMappingsLock = Promise.resolve();
    function applyToneMappingsNow(songKey, options = {}) {
        const next = _applyToneMappingsLock
            .catch(() => {})
            .then(() => _applyToneMappingsImpl(songKey, options));
        _applyToneMappingsLock = next.catch(() => {});
        return next;
    }

    async function _applyToneMappingsImpl(songKey, options = {}) {
        const forceBypass = !!options.forceBypass;
        const changedTone = String(options.changedTone || '').trim();
        const hw = window.highway || window._slopsmithHighway;
        if (!hw) return;

        // Tone Automation overrides manual tone mappings: it owns the chain
        // while enabled and reacts to song/tone events through its own switcher.
        if (!options.force && window._aeToneAutomation?.isEnabled?.()) {
            let rawToneChanges = [], rawToneBase = '';
            try {
                rawToneChanges = hw.getToneChanges ? hw.getToneChanges() : [];
                rawToneBase = hw.getToneBase ? hw.getToneBase() : '';
            } catch (e) {
                console.warn('[tone-switcher] Failed to read highway tone data (TA path):', e);
            }
            await installTaSwitcherForSong(songKey, rawToneChanges, rawToneBase);
            return;
        }

        let rawToneChanges = [], rawToneBase = '';
        try {
            rawToneChanges = hw.getToneChanges ? hw.getToneChanges() : [];
            rawToneBase = hw.getToneBase ? hw.getToneBase() : '';
        } catch (e) {
            console.warn('[tone-switcher] Failed to read highway tone data:', e);
        }
        const arrangementName = getCurrentArrangementName();
        const normalized = await normalizeTimelineToneData(songKey, rawToneChanges, rawToneBase, arrangementName);
        const toneChanges = normalized.toneChanges;
        const toneBase = normalized.toneBase;
        const mappings = getToneMappings(songKey);
        if (Object.keys(mappings).length === 0) {
            // No mappings for this song — tear down any switcher/monitor left over from a
            // previous arrangement so it doesn't keep acting on stale tone-change data.
            window._toneSwitcher = null;
            window._aeStopToneMonitor?.();
            return;
        }
        if (toneChanges.length === 0 && !toneBase) {
            // Songs without tone automation: apply the selected tone mapping directly.
            const arrangementToneNames = normalized.originalNames.length > 0
                ? normalized.originalNames
                : await getOriginalToneNames(songKey, arrangementName);
            const targetTone =
                changedTone ||
                arrangementToneNames.find(n => !!findMappingForTone(mappings, n)) ||
                Object.keys(mappings).filter(k => k !== '$default')[0] ||
                '';
            const presetName = resolveTonePresetName(mappings, targetTone);
            const presets = getPresets();
            const preset = presetName ? presets[presetName] : null;
            if (preset?.nativePreset) {
                await replaceChainWithPresetBlob(preset, `tone-map:${songKey}`, { snapshot: false });
            } else {
                await loadDefaultPreset('tone-none');
            }
            return;
        }

        const currentTime = hw.getTime ? hw.getTime() : 0;
        const activeNow = getActiveToneAtTime(currentTime, toneChanges, toneBase);

        if (!forceBypass) {
            const midiConfig = getMidiPCConfig(songKey);
            if (midiConfig?.mode === 'midi' && midiConfig.vstSlotId >= 0) {
                const midiMappings = midiConfig.mappings || {};
                toneSwitcher = null;
                window._toneSwitcher = {
                    activeTone: null,
                    midiMode: true,
                    switchToTone(name) {
                        if (name === this.activeTone) return;
                        const program = midiMappings[name];
                        if (program !== undefined && api?.sendMidiToSlot) {
                            api.sendMidiToSlot(midiConfig.vstSlotId, 0, midiConfig.channel || 1, program);
                        }
                        this.activeTone = name;
                    }
                };
                if (activeNow) window._toneSwitcher.switchToTone(activeNow);
                return;
            }
        }

        toneSwitcher = new ToneSwitcher();
        window._toneSwitcher = toneSwitcher;
        await toneSwitcher.preloadForSong(toneChanges, toneBase, mappings);
        if (activeNow) toneSwitcher.switchToTone(activeNow);
        if (autoSwitchEnabled) startToneMonitor();
    }

    function stopToneMonitor() {
        if (hookState.toneMonitorInterval) { clearInterval(hookState.toneMonitorInterval); hookState.toneMonitorInterval = null; }
    }

    // ── Floating Tone Panel in Player ──────────────────────────────────────────
    function injectPlayerToneButton() {
        const controls = document.getElementById('player-controls');
        if (!controls || document.getElementById('btn-chain-switch')) return;

        // Add button before the close button
        const closeBtn = controls.querySelector('button[onclick*="showScreen"]');
        if (closeBtn && !closeBtn.dataset.chainPanelCloseBound) {
            closeBtn.addEventListener('click', () => {
                closeTonePanel();
                void loadDefaultPreset('player-exit');
            }, { capture: true });
            closeBtn.dataset.chainPanelCloseBound = '1';
        }
        const btn = document.createElement('button');
        btn.id = 'btn-chain-switch';
        btn.className = 'px-3 py-1.5 bg-orange-900/40 hover:bg-orange-900/60 rounded-lg text-xs text-orange-300 transition';
        btn.textContent = 'Chain';
        btn.onclick = () => toggleTonePanel();
        if (closeBtn) controls.insertBefore(btn, closeBtn);
        else controls.appendChild(btn);
    }

    window._toggleChainPanel = toggleTonePanel;
    function closeTonePanel() {
        const panel = document.getElementById('ae-tone-panel-float');
        if (panel?._aeActiveToneInterval) {
            clearInterval(panel._aeActiveToneInterval);
            panel._aeActiveToneInterval = null;
        }
        if (panel) panel.remove();
    }
    window._closeChainPanel = closeTonePanel;
    async function refreshTonePanelIfOpen() {
        const panel = document.getElementById('ae-tone-panel-float');
        if (!panel) return;
        closeTonePanel();
        await toggleTonePanel();
    }
    window._refreshChainPanel = refreshTonePanelIfOpen;

    async function toggleTonePanel() {
        let panel = document.getElementById('ae-tone-panel-float');
        if (panel) { closeTonePanel(); return; }

        const player = document.getElementById('player');
        if (!player) return;

        // Show panel immediately with loading state
        panel = document.createElement('div');
        panel.id = 'ae-tone-panel-float';
        panel.style.cssText = 'position:absolute;bottom:60px;right:12px;z-index:100;width:320px;max-height:400px;overflow-y:auto;';
        panel.className = 'bg-slate-900 border border-slate-700 rounded-xl p-4 shadow-2xl';
        panel.innerHTML = `<div class="flex items-center justify-between mb-3">
            <span class="text-sm font-semibold text-slate-200">Tone Switching</span>
            <button type="button" onclick="window._closeChainPanel && window._closeChainPanel()" class="text-slate-500 hover:text-white text-lg leading-none">&times;</button>
        </div><div class="text-xs text-slate-400 animate-pulse">Loading...</div>`;
        player.style.position = 'relative';
        player.appendChild(panel);

        let toneChanges = [];
        let toneBase = '';
        let presets = {};
        let presetNames = [];
        let songKey = '';
        let mappings = {};
        let midiConfig = null;
        let isMidiMode = false;
        const toneNamesOrdered = [];
        const addToneUnique = (name) => {
            const n = String(name || '').trim();
            if (!n || toneNamesOrdered.includes(n)) return;
            toneNamesOrdered.push(n);
        };

        try {
            const hw = window.highway || window._slopsmithHighway;
            try {
                toneChanges = hw?.getToneChanges ? hw.getToneChanges() : [];
            } catch (e) {
                console.warn('[audio-engine] getToneChanges failed:', e);
            }
            let rawToneBaseFromHw = '';
            try {
                toneBase = hw?.getToneBase ? hw.getToneBase() : '';
                rawToneBaseFromHw = toneBase;
            } catch (e) {
                console.warn('[audio-engine] getToneBase failed:', e);
            }
            try {
                presets = getPresets();
            } catch (e) {
                console.warn('[audio-engine] slopsmith-chain-presets JSON invalid, using {}:', e);
                presets = {};
            }
            presetNames = Object.keys(presets);
            songKey = getCurrentSongKey();
            try {
                mappings = getToneMappings(songKey);
            } catch (e) {
                console.warn('[audio-engine] tone mappings JSON invalid:', e);
                mappings = {};
            }
            try {
                midiConfig = getMidiPCConfig(songKey);
            } catch (e) {
                console.warn('[audio-engine] MIDI PC config read failed:', e);
                midiConfig = null;
            }
            isMidiMode = midiConfig?.mode === 'midi';

            const arrangementName = getCurrentArrangementName();
            const normalized = await normalizeTimelineToneData(songKey, toneChanges, toneBase, arrangementName);
            toneChanges = normalized.toneChanges;
            toneBase = normalized.toneBase;
            toneNamesOrdered.length = 0;
            // Timeline has only a base tone (no tone_changes): show one row — do not list tones from other arrangements.
            if (toneChanges.length === 0 && (toneBase || rawToneBaseFromHw)) {
                addToneUnique(toneBase || rawToneBaseFromHw);
            } else {
                if (toneBase) addToneUnique(toneBase);
                for (const tc of toneChanges) {
                    if (tc && tc.name) addToneUnique(tc.name);
                }
                if (toneNamesOrdered.length === 0) {
                    for (const n of normalized.originalNames) addToneUnique(n);
                }
            }
        } catch (err) {
            console.error('[audio-engine] Failed to read tone / preset data:', err);
            const stillThere = document.getElementById('ae-tone-panel-float');
            if (stillThere) {
                stillThere.innerHTML = `<div class="flex items-center justify-between mb-3">
                    <span class="text-sm font-semibold text-slate-200">Tone Switching</span>
                    <button type="button" onclick="window._closeChainPanel && window._closeChainPanel()" class="text-slate-500 hover:text-white text-lg leading-none">&times;</button>
                </div>
                <div class="text-xs text-red-400">Failed to load: ${escHtml((err && err.message) || 'unknown error')}</div>`;
            }
            return;
        }

        if (!document.getElementById('ae-tone-panel-float')) return;

        try {

        // VST list is filled after first paint. Calling getChainState() before
        // innerHTML can freeze the UI forever if the native bridge blocks the JS
        // thread synchronously (timers then never run).
        const midiMappings = midiConfig?.mappings || {};
        const taCfg = readTaStore();
        /** Which switching UI is active: automation wins over MIDI when enabled in settings. */
        const panelMode = taCfg.enabled ? 'automation' : (isMidiMode ? 'midi' : 'bypass');

        let html = `<div class="flex items-center justify-between mb-3">
            <span class="text-sm font-semibold text-slate-200">Tone Switching</span>
            <button type="button" onclick="window._closeChainPanel && window._closeChainPanel()" class="text-slate-500 hover:text-white text-lg leading-none">&times;</button>
        </div>`;

        if (toneNamesOrdered.length === 0) {
            const arrangementName = getCurrentArrangementName();
            const originalNames = await getOriginalToneNames(songKey, arrangementName);
            for (const n of originalNames) addToneUnique(n);
        }

        // Mode selector — Preset Switch | MIDI PC | Tone Automation (keyword routing from settings)
        html += `<div class="flex items-center gap-2 mb-3">
            <label class="text-xs text-slate-400">Mode:</label>
            <select id="ae-tone-mode" class="flex-1 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300">
                <option value="bypass" ${panelMode === 'bypass' ? 'selected' : ''}>Preset Switch</option>
                <option value="midi" ${panelMode === 'midi' ? 'selected' : ''}>MIDI Program Change</option>
                <option value="automation" ${panelMode === 'automation' ? 'selected' : ''}>Tone Automation</option>
            </select>
        </div>`;

        if (toneNamesOrdered.length > 0) {

            // Bypass mode — manual preset mapping per tone name
            html += `<div id="ae-bypass-mode" class="${panelMode === 'bypass' ? '' : 'hidden'}">`;
            html += '<div class="space-y-2 mb-3">';
            for (const tone of toneNamesOrdered) {
                if (!tone) continue;
                const mappedPreset = findMappingForTone(mappings, tone) || mappings[tone] || null;
                html += `<div class="flex items-center gap-2">
                    <span class="text-xs text-slate-400 w-24 truncate" title="${escHtml(tone)}">${escHtml(tone)}</span>
                    <select class="flex-1 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300" data-tone="${escHtml(tone)}">
                        <option value="">-- none --</option>
                        ${presetNames.map(p => `<option value="${escHtml(p)}" ${mappedPreset === p ? 'selected' : ''}>${escHtml(p)}</option>`).join('')}
                    </select>
                </div>`;
            }
            html += '</div></div>';

            // Tone Automation — classifier defaults + optional session-only overrides (see *)
            html += `<div id="ae-automation-mode" class="${panelMode === 'automation' ? '' : 'hidden'}">`;
            html += `<p class="text-[11px] text-slate-500 mb-2 leading-snug">Targets from keywords (configure in audio settings). You may override presets per tone for this play only; <span class="text-slate-400">*</span> means different from automation. Closing the song resets overrides.</p>`;
            html += '<div class="space-y-2 mb-3">';
            for (const tone of toneNamesOrdered) {
                if (!tone) continue;
                const eff = getTaSessionEffectivePreset(tone, taCfg);
                const effPreset = eff.effectiveName || '';
                const catUpper = String(eff.category || '').toUpperCase();
                html += `<div class="flex items-center gap-2">
                    <span class="text-xs text-slate-400 w-24 truncate" title="${escHtml(tone)}">${escHtml(tone)}</span>
                    <select data-ta-override-tone="${escHtml(tone)}" class="flex-1 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300">
                        <option value="">-- none --</option>
                        ${presetNames.map(p => `<option value="${escHtml(p)}" ${effPreset === p ? 'selected' : ''}>${escHtml(p)}</option>`).join('')}
                    </select>
                    <span data-ta-override-star class="text-amber-400 w-4 shrink-0 text-center text-sm font-bold leading-none" title="Preset differs from automation (this session only)">${eff.showStar ? '*' : ''}</span>
                    <span class="text-[10px] text-emerald-400/90 w-11 shrink-0 text-right font-medium" title="Classifier bucket">${escHtml(catUpper)}</span>
                </div>`;
            }
            html += '</div></div>';

            // MIDI PC mode — VST dropdown is filled after first paint (see hydrateChainVstOptions)
            html += `<div id="ae-midi-mode" class="${panelMode === 'midi' ? '' : 'hidden'}">`;
            html += `<div class="space-y-2 mb-3">
                <div class="flex items-center gap-2">
                    <span class="text-xs text-slate-400 w-20">VST:</span>
                    <select id="ae-midi-vst" class="flex-1 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300">
                        <option value="">Loading plug-in list…</option>
                    </select>
                </div>
                <div class="flex items-center gap-2">
                    <span class="text-xs text-slate-400 w-20">Channel:</span>
                    <input type="number" id="ae-midi-ch" min="1" max="16" value="${midiConfig?.channel || 1}" class="w-16 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300">
                </div>
            </div>`;
            html += '<div id="ae-midi-vst-hint" class="text-xs text-slate-500 mb-2"></div>';
            html += '<div class="space-y-1 mb-3">';
            for (const tone of toneNamesOrdered) {
                if (!tone) continue;
                html += `<div class="flex items-center gap-2">
                    <span class="text-xs text-slate-400 w-24 truncate" title="${escHtml(tone)}">${escHtml(tone)}</span>
                    <input type="number" min="0" max="127" value="${midiMappings[tone] ?? ''}" placeholder="PC#"
                        data-midi-tone="${escHtml(tone)}" class="w-16 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-300">
                </div>`;
            }
            html += '</div>';
            html += `<button id="ae-midi-save" class="px-3 py-1.5 rounded bg-emerald-600/50 hover:bg-emerald-500 text-xs text-slate-200">Save MIDI Mapping</button>`;
            html += '</div>';
        } else {
            // Keep mode-section ids so the Mode dropdown can show/hide bodies even
            // when this arrangement exposes no tone rows yet.
            html += `<div id="ae-bypass-mode" class="${panelMode === 'bypass' ? '' : 'hidden'}"><p class="text-xs text-slate-500 italic">No tone information found for this song.</p></div>`;
            html += `<div id="ae-automation-mode" class="${panelMode === 'automation' ? '' : 'hidden'}"><p class="text-xs text-slate-500 italic mb-1">No tone names listed for this arrangement.</p><p class="text-[11px] text-slate-600">Automation still classifies from the song title when you play.</p></div>`;
            html += `<div id="ae-midi-mode" class="${panelMode === 'midi' ? '' : 'hidden'}"><p class="text-xs text-slate-500 italic">No tones to assign MIDI program numbers.</p></div>`;
        }

        html += `<label class="flex items-center gap-2 text-xs text-slate-400 cursor-pointer mb-2 mt-2">
            <input type="checkbox" class="accent-blue-500" id="ae-float-auto-switch" ${autoSwitchEnabled ? 'checked' : ''}>
            Auto-switch during playback
        </label>`;
        html += `<div class="mt-3 pt-2 border-t border-slate-700/60">
            <div class="text-[10px] uppercase tracking-wider text-slate-500 font-semibold mb-1">Active Indicator</div>
            <div class="text-xs text-slate-300 font-medium min-h-[1rem]" id="ae-active-tone">Active: —</div>
        </div>`;

        panel.innerHTML = html;
        player.style.position = 'relative';
        player.appendChild(panel);

        // Active indicator — shows the live tone state resolved through whichever
        // engine is currently driving `_toneSwitcher` (Tone Automation, manual
        // bypass mappings, or MIDI PC). When a preset is known, it's rendered as
        // `tone → preset` so users can verify their classification visually.
        const updateActiveToneLabel = () => {
            const el = document.getElementById('ae-active-tone');
            if (!el) return;
            const hw = window.highway || window._slopsmithHighway;
            const switcher = window._toneSwitcher;
            // Read the TA store once per tick and derive taOn from it so the 200ms
            // interval doesn't call readTaStore() (JSON.parse) more than once.
            const taCfg = readTaStore();
            const taOn = !!taCfg.enabled;

            let tNum = 0, changes = [], base = '';
            try {
                tNum = (hw && typeof hw.getTime === 'function') ? hw.getTime() : 0;
                changes = (hw && hw.getToneChanges) ? hw.getToneChanges() : [];
                base = (hw && hw.getToneBase) ? hw.getToneBase() : '';
            } catch (e) {
                // Highway not ready or threw; use empty tone data for this tick
            }
            const timelineTone = getActiveToneAtTime(tNum, changes, base);
            const switcherTone = String(switcher?.activeTone || '').trim();
            const activeTone = switcherTone || timelineTone || '';

            let presetName = '';
            if (switcher) {
                if (switcher.activePreset) {
                    presetName = String(switcher.activePreset);
                } else if (switcher.tonePresetNameMap && activeTone && switcher.tonePresetNameMap[activeTone]) {
                    // Use the name map directly — getPresets() re-parses JSON on every call so
                    // object-reference equality against tonePresetMap entries never matches.
                    presetName = switcher.tonePresetNameMap[activeTone];
                }
            }
            if (!presetName && taOn && activeTone) {
                const eff = getTaSessionEffectivePreset(activeTone, taCfg);
                if (eff.effectiveName) presetName = eff.effectiveName;
            }

            const prefix = taOn ? 'Active (TA):' : 'Active:';
            if (activeTone && presetName) {
                el.innerHTML = `<span class="text-slate-500">${prefix}</span> <span>${escHtml(activeTone)}</span> <span class="text-slate-500">→</span> <span class="text-blue-300">${escHtml(presetName)}</span>`;
            } else if (activeTone) {
                el.innerHTML = `<span class="text-slate-500">${prefix}</span> <span>${escHtml(activeTone)}</span>`;
            } else if (presetName) {
                el.innerHTML = `<span class="text-slate-500">${prefix}</span> <span class="text-blue-300">${escHtml(presetName)}</span>`;
            } else {
                el.innerHTML = `<span class="text-slate-500">${prefix}</span> <span class="text-slate-500">—</span>`;
            }
        };
        updateActiveToneLabel();
        panel._aeActiveToneInterval = setInterval(updateActiveToneLabel, 200);

        // Wire up select changes
        panel.querySelectorAll('select[data-tone]').forEach(sel => {
            sel.addEventListener('change', async (e) => {
                // Read the per-song bucket directly — getToneMappings() returns a merged
                // {global, ...song} object, and writing it back would bake global entries
                // into the song bucket, silently shadowing future global-mapping edits.
                const songBucket = { ...(readToneMappingsStore().songs[songKey] || {}) };
                if (e.target.value) songBucket[e.target.dataset.tone] = e.target.value;
                else delete songBucket[e.target.dataset.tone];
                saveToneMappings(songKey, songBucket);
                // Force bypass preloader to rebuild mapping during current playback
                window._toneMappingsDirty = true;
                try {
                    await applyToneMappingsNow(songKey, { forceBypass: true, changedTone: e.target.dataset.tone });
                } catch (err) {
                    console.error('[tone-switcher] Failed to apply mapping live:', err);
                }
            });
        });

        panel.querySelectorAll('select[data-ta-override-tone]').forEach(sel => {
            sel.addEventListener('change', async (e) => {
                const tone = e.target.dataset.taOverrideTone;
                const val = e.target.value;
                const cfg = readTaStore();
                const { presetName: autoP } = resolveTaPreset(tone, cfg);
                window._aeTaSessionOverrides = window._aeTaSessionOverrides || {};
                if (!val || val === autoP) delete window._aeTaSessionOverrides[tone];
                else window._aeTaSessionOverrides[tone] = val;

                const starEl = e.target.parentElement?.querySelector('[data-ta-override-star]');
                if (starEl) {
                    const eff = getTaSessionEffectivePreset(tone, readTaStore());
                    starEl.textContent = eff.showStar ? '*' : '';
                }

                const switcher = window._toneSwitcher;
                if (!switcher?.taSwitcher) return;
                const hw = window.highway || window._slopsmithHighway;
                const tNum = hw?.getTime ? hw.getTime() : 0;
                const changes = hw?.getToneChanges ? hw.getToneChanges() : [];
                const base = hw?.getToneBase ? hw.getToneBase() : '';
                const timelineTone = getActiveToneAtTime(tNum, changes, base);
                const switcherTone = String(switcher.activeTone || '').trim();
                const activeToneNow = switcherTone || timelineTone || '';
                if (String(activeToneNow) !== String(tone)) return;

                const eff = getTaSessionEffectivePreset(tone, readTaStore());
                const toLoad = eff.effectiveName;
                if (!toLoad) return;
                const ok = await loadPresetByName(toLoad);
                if (ok) switcher.activePreset = toLoad;
            });
        });

        // Mode dropdown — Preset Switch / MIDI Program Change / Tone Automation.
        // Tone Automation mode toggles `slopsmith-tone-automation.enabled` (settings UI has no duplicate toggle).
        const modeSelect = panel.querySelector('#ae-tone-mode');
        if (modeSelect) {
            modeSelect.addEventListener('change', async () => {
                const v = modeSelect.value;
                const bypassDiv = panel.querySelector('#ae-bypass-mode');
                const midiDiv = panel.querySelector('#ae-midi-mode');
                const autoDiv = panel.querySelector('#ae-automation-mode');

                if (v === 'midi') {
                    bypassDiv?.classList.add('hidden');
                    autoDiv?.classList.add('hidden');
                    midiDiv?.classList.remove('hidden');
                    const cur = readTaStore();
                    if (cur.enabled) {
                        cur.enabled = false;
                        writeTaStore(cur);
                        // Reuse deactivateToneAutomation() so the loadDefaultPreset fallback
                        // is conditional on whether manual mappings exist — avoids overwriting
                        // a chain that applyToneMappingsNow just configured for MIDI mode.
                        await deactivateToneAutomation();
                    }
                    window._toneMappingsDirty = true;
                } else if (v === 'automation') {
                    bypassDiv?.classList.add('hidden');
                    midiDiv?.classList.add('hidden');
                    autoDiv?.classList.remove('hidden');
                    saveMidiPCConfig(songKey, null);
                    const cur = readTaStore();
                    cur.enabled = true;
                    writeTaStore(cur);
                    window._toneMappingsDirty = true;
                    await activateToneAutomationForCurrentSong();
                    // Ensure the IIFE2 tone-change poller is running so TA reacts to
                    // subsequent tone changes immediately when enabled mid-song.
                    window._aeStartToneAutoSwitch?.();
                } else {
                    // Preset Switch (bypass)
                    bypassDiv?.classList.remove('hidden');
                    midiDiv?.classList.add('hidden');
                    autoDiv?.classList.add('hidden');
                    saveMidiPCConfig(songKey, null);
                    const cur = readTaStore();
                    if (cur.enabled) {
                        cur.enabled = false;
                        writeTaStore(cur);
                        await deactivateToneAutomation();
                    }
                    window._toneMappingsDirty = true;
                }
            });
        }

        // Wire MIDI save button
        const midiSaveBtn = panel.querySelector('#ae-midi-save');
        if (midiSaveBtn) {
            midiSaveBtn.addEventListener('click', () => {
                const vstSelect = panel.querySelector('#ae-midi-vst');
                const chInput = panel.querySelector('#ae-midi-ch');
                const midiInputs = panel.querySelectorAll('[data-midi-tone]');
                const mappingsObj = {};
                midiInputs.forEach(inp => {
                    if (inp.value !== '') mappingsObj[inp.dataset.midiTone] = parseInt(inp.value);
                });
                saveMidiPCConfig(songKey, {
                    mode: 'midi',
                    vstSlotId: vstSelect ? parseInt(vstSelect.value) : -1,
                    channel: chInput ? parseInt(chInput.value) : 1,
                    mappings: mappingsObj,
                });
                // Apply MIDI mode immediately
                window._toneMappingsDirty = true;
                const _liveApi = window.slopsmithDesktop?.audio;
                const _midiMappings = mappingsObj;
                const _midiVstSlot = vstSelect ? parseInt(vstSelect.value) : -1;
                const _midiCh = chInput ? parseInt(chInput.value) : 1;
                window._toneSwitcher = {
                    activeTone: null,
                    midiMode: true,
                    switchToTone(name) {
                        if (name === this.activeTone) return;
                        const program = _midiMappings[name];
                        if (program !== undefined && _liveApi?.sendMidiToSlot) {
                            _liveApi.sendMidiToSlot(_midiVstSlot, 0, _midiCh, program);
                            console.log('[tone-switcher] MIDI PC:', name, '-> program', program);
                        }
                        this.activeTone = name;
                    }
                };
                console.log('[tone-switcher] Saved & activated MIDI config:', mappingsObj);
                midiSaveBtn.textContent = 'Saved!';
                setTimeout(() => { midiSaveBtn.textContent = 'Save MIDI Mapping'; }, 1500);
            });
        }

        // Wire auto-switch checkbox
        const cb = panel.querySelector('#ae-float-auto-switch');
        if (cb) cb.addEventListener('change', () => {
            autoSwitchEnabled = cb.checked;
            localStorage.setItem('slopsmith-tone-auto-switch', String(autoSwitchEnabled));
            if (!autoSwitchEnabled) stopToneMonitor();
        });

        // Fill VST dropdown after UI is visible. getChainState() may block the JS thread
        // synchronously; deferring avoids an endless "Loading..." on the whole panel.
        void (async function hydrateChainVstOptions() {
            const root = document.getElementById('ae-tone-panel-float');
            if (!root) return;
            const vstSelect = root.querySelector('#ae-midi-vst');
            const hint = root.querySelector('#ae-midi-vst-hint');
            if (!vstSelect) return;
            const apiLocal = window.slopsmithDesktop?.audio;
            if (!apiLocal || typeof apiLocal.getChainState !== 'function') {
                vstSelect.innerHTML = '<option value="">(no audio bridge)</option>';
                return;
            }
            // Yield to the event loop so the browser can paint before we call into the
            // native bridge. getChainState() may block the JS thread synchronously, and
            // wrapping it in Promise.resolve() doesn't help — the call still evaluates
            // before any await. Yielding first ensures the UI is visible before the block.
            await new Promise(r => setTimeout(r, 0));
            let vstSlots = [];
            try {
                const chain = await Promise.race([
                    apiLocal.getChainState(),
                    new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 5000))
                ]);
                if (Array.isArray(chain)) vstSlots = chain.filter(s => s.type === 0);
            } catch (e) {
                console.warn('[audio-engine] getChainState failed:', e.message || e);
                vstSelect.innerHTML = '<option value="">(unavailable)</option>';
                if (hint) hint.textContent = 'Could not load the VST list. Check the audio engine or try again.';
                return;
            }
            let cfg = null;
            try {
                cfg = getMidiPCConfig(songKey);
            } catch (e) { /* ignore */ }
            if (vstSlots.length === 0) {
                vstSelect.innerHTML = '<option value="">(no VST in chain)</option>';
                if (hint) hint.textContent = 'Load a VST in the audio panel, then reopen this panel.';
            } else {
                const esc = (t) => String(t ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
                vstSelect.innerHTML = vstSlots.map(s =>
                    `<option value="${s.id}" ${cfg?.vstSlotId === s.id ? 'selected' : ''}>${esc(s.name)}</option>`
                ).join('');
                if (hint) hint.textContent = '';
            }
        })();

        } catch (err) {
            console.error('[audio-engine] Failed to render tone panel:', err);
            const stillThere = document.getElementById('ae-tone-panel-float');
            if (stillThere) {
                stillThere.innerHTML = `<div class="flex items-center justify-between mb-3">
                    <span class="text-sm font-semibold text-slate-200">Tone Switching</span>
                    <button type="button" onclick="window._closeChainPanel && window._closeChainPanel()" class="text-slate-500 hover:text-white text-lg leading-none">&times;</button>
                </div>
                <div class="text-xs text-red-400">Failed to load: ${escHtml((err && err.message) || 'unknown error')}</div>`;
            }
        }
    }

    // Hook playSong for tone switching setup. Keep the wrapper singleton-style so
    // renderer rehydration updates this implementation without stacking wrappers.
    hookState.toneSetupImpl = async function(filename, arrangement, nextPlaySong) {
        if (window._aeMarkSongTransition) window._aeMarkSongTransition(7000);
        stopToneMonitor();
        closeTonePanel();
        await nextPlaySong.call(this, filename, arrangement);
        // Inject tones button into player controls
        setTimeout(() => injectPlayerToneButton(), 500);
        // Tone-chain preload is handled only by the outer playSong timer (single path — avoids
        // stacking the menu chain with a second preload).
    };
    if (typeof window.playSong === 'function' && !hookState.toneSetupInstalled) {
        hookState.toneSetupBasePlaySong = window.playSong;
        window.playSong = async function(filename, arrangement) {
            return hookState.toneSetupImpl.call(this, filename, arrangement, hookState.toneSetupBasePlaySong);
        };
        hookState.toneSetupInstalled = true;
    }

    // ── Tone Automation ────────────────────────────────────────────────────────
    /** Default keyword dictionaries for the Auto Classifier Filters. Each
     *  category accepts user-supplied custom keywords on top of these. */
    const TA_DEFAULT_KEYWORDS = {
        solo:     ['lead', 'solo'],
        dist:     ['dist', 'distortion', 'fuzz', 'gain', 'higain', 'highgain', 'dis'],
        od:       ['overdrive', 'od', 'drive', 'crunch', 'dirty', 'breakup', 'over'],
        clean:    ['clean', 'twang', 'chime', 'sparkle'],
        bass:     ['bass', 'bass gtr', 'bassgtr', 'bs'],
        acoustic: ['acoustic', 'acous', 'acc', 'ac'],
        mod:      ['wah', 'chorus', 'verb', 'reverb', 'delay', 'echo', 'trem', 'tremolo',
                   'phase', 'phaser', 'flange', 'flanger', 'filter', 'mod', 'fx'],
    };

    /** Highest priority wins when an input matches more than one category. */
    const TA_PRECEDENCE = ['solo', 'dist', 'od', 'clean', 'bass', 'acoustic', 'mod'];

    const TA_CATEGORY_LABELS = {
        solo: 'Solo', dist: 'Dist', od: 'OD',
        clean: 'Clean', bass: 'Bass', acoustic: 'Acoustic', mod: 'Mod',
    };

    const TA_TARGET_ROWS = [
        { key: 'clean',    label: 'Auto Clean Target' },
        { key: 'bass',     label: 'Auto Bass Target' },
        { key: 'acoustic', label: 'Auto Acoustic Target' },
        { key: 'od',       label: 'Auto OD Target' },
        { key: 'dist',     label: 'Auto Dist Target' },
        { key: 'mod',      label: 'Auto Mod Target' },
        { key: 'solo',     label: 'Auto Solo Target' },
        { key: 'idle',     label: 'Idle Target (fallback)' },
    ];

    function readTaStore() {
        try {
            const raw = JSON.parse(localStorage.getItem('slopsmith-tone-automation') || '{}') || {};
            const customKeywords = (raw.customKeywords && typeof raw.customKeywords === 'object' && !Array.isArray(raw.customKeywords)) ? raw.customKeywords : {};
            const targets = (raw.targets && typeof raw.targets === 'object' && !Array.isArray(raw.targets)) ? raw.targets : {};
            return { enabled: !!raw.enabled, customKeywords, targets };
        } catch (e) {
            return { enabled: false, customKeywords: {}, targets: {} };
        }
    }

    function writeTaStore(cfg) {
        localStorage.setItem('slopsmith-tone-automation', JSON.stringify({
            enabled: !!cfg?.enabled,
            customKeywords: cfg?.customKeywords && typeof cfg.customKeywords === 'object' ? cfg.customKeywords : {},
            targets: cfg?.targets && typeof cfg.targets === 'object' ? cfg.targets : {},
        }));
    }

    function parseTaKeywordList(value) {
        if (!value) return [];
        if (Array.isArray(value)) return value.map(v => String(v).trim()).filter(Boolean);
        return String(value).split(/[,\n;]+/).map(v => v.trim()).filter(Boolean);
    }

    function getTaCategoryKeywords(category, cfg) {
        const c = cfg || readTaStore();
        const defaults = TA_DEFAULT_KEYWORDS[category] || [];
        const custom = parseTaKeywordList(c.customKeywords?.[category]);
        return [...defaults, ...custom];
    }

    /** Builds a case-insensitive regex that matches any keyword anchored at a
     *  left word boundary (start of string or non-alphanumeric). The right side
     *  stays open so 'dist' matches 'distortion' and 'mod' matches 'modulator'.
     *  Longer keywords are tried first so 'distortion' wins over 'dist'.
     *  Results are memoized so callers on a tight interval (e.g. 200ms label update)
     *  don't re-compile identical regexes on every tick. */
    const _taRegexCache = new Map();
    function buildTaKeywordRegex(keywords) {
        const cleaned = (keywords || [])
            .map(k => String(k || '').trim().toLowerCase())
            .filter(Boolean);
        if (cleaned.length === 0) return null;
        cleaned.sort((a, b) => b.length - a.length);
        const cacheKey = cleaned.join('\x00');
        if (_taRegexCache.has(cacheKey)) return _taRegexCache.get(cacheKey);
        const escaped = cleaned.map(k => k.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
        const re = new RegExp(`(?:^|[^a-z0-9])(?:${escaped.join('|')})`, 'i');
        if (_taRegexCache.size >= 50) _taRegexCache.delete(_taRegexCache.keys().next().value);
        _taRegexCache.set(cacheKey, re);
        return re;
    }

    function classifyTaCategory(input, cfg) {
        const text = String(input || '').toLowerCase();
        if (!text) return null;
        const c = cfg || readTaStore();
        for (const cat of TA_PRECEDENCE) {
            const re = buildTaKeywordRegex(getTaCategoryKeywords(cat, c));
            if (re && re.test(text)) return cat;
        }
        return null;
    }

    function resolveTaPreset(input, cfg) {
        const c = cfg || readTaStore();
        const cat = classifyTaCategory(input, c);
        const targets = c.targets || {};
        if (cat && targets[cat]) return { presetName: targets[cat], category: cat };
        return { presetName: targets.idle || null, category: 'idle' };
    }

    /** Session-only Chain overrides per tone (Tone Automation). Cleared on each new `playSong`. */
    if (typeof window._aeTaSessionOverrides !== 'object' || window._aeTaSessionOverrides === null) {
        window._aeTaSessionOverrides = {};
    }

    /** Effective preset for a tone name: classifier target ± optional modal override for this playback. */
    function getTaSessionEffectivePreset(toneName, cfg) {
        const t = String(toneName || '').trim();
        const c = cfg || readTaStore();
        const autoResolved = resolveTaPreset(t, c);
        const autoName = autoResolved.presetName || null;
        const ov = window._aeTaSessionOverrides;
        if (ov && Object.prototype.hasOwnProperty.call(ov, t)) {
            const m = ov[t];
            const effectiveName = (m !== undefined && m !== '') ? m : autoName;
            const showStar = effectiveName !== autoName;
            return {
                autoName,
                effectiveName,
                showStar,
                category: autoResolved.category,
            };
        }
        return {
            autoName,
            effectiveName: autoName,
            showStar: false,
            category: autoResolved.category,
        };
    }

    async function loadPresetByName(presetName) {
        if (!presetName) return false;
        const presets = getPresets();
        const preset = presets[presetName];
        if (!preset?.nativePreset) return false;
        const ok = await replaceChainWithPresetBlob(preset, `tone-automation:${presetName}`, { snapshot: false });
        if (!ok) console.error('[tone-automation] Failed to load preset:', presetName);
        return ok;
    }

    /** Drop-in replacement for `_toneSwitcher` that classifies tone names through
     *  the Tone Automation dictionaries and loads the matching target preset.
     *  Repeated calls that resolve to the same preset are no-ops to keep the
     *  audio chain stable while crossing tone-change boundaries. */
    class ToneAutomationSwitcher {
        constructor() {
            this.activeTone = null;
            this.activePreset = null;
            this.taSwitcher = true;
            this._switchLock = null;  // in-flight Promise; null when idle
            this._pendingTone = null; // latest request that arrived while locked (coalesced)
        }
        async switchToTone(name) {
            const input = String(name || '').trim();
            if (!input) return;
            this.activeTone = input;
            if (this._switchLock) {
                // A switch is already in progress — record latest request and let it drain.
                this._pendingTone = input;
                return;
            }
            // Drain: process the current request, then any coalesced pending one.
            let current = input;
            while (current) {
                this._pendingTone = null;
                this._switchLock = this._doSwitch(current);
                await this._switchLock;
                this._switchLock = null;
                current = this._pendingTone;
            }
        }
        async _doSwitch(input) {
            const cfg = readTaStore();
            const { effectiveName: presetName, autoName, category } = getTaSessionEffectivePreset(input, cfg);
            if (!presetName || presetName === this.activePreset) return;
            const ok = await loadPresetByName(presetName);
            if (ok) {
                this.activePreset = presetName;
                const tag = autoName !== presetName ? ' (session override)' : '';
                console.log('[tone-automation] tone:', input, '→', category, '→', presetName, tag);
            }
        }
    }

    /** Applies the appropriate target preset to a free-form input string (song
     *  filename, tone name, or manual user text). When `force` is set, ignores
     *  the enabled toggle so the modal's "Apply" button can still trigger. */
    async function applyToneAutomationFor(input, options = {}) {
        const cfg = readTaStore();
        if (!cfg.enabled && !options.force) return false;
        const { presetName, category } = resolveTaPreset(input, cfg);
        if (!presetName) return false;
        const ok = await loadPresetByName(presetName);
        if (ok) console.log('[tone-automation] input:', input, '→', category, '→', presetName);
        return ok;
    }

    /** Installs a TA-driven `_toneSwitcher` for the loaded song. Called from
     *  the playSong wrapper when Tone Automation Mode is on. Picks the initial
     *  preset from the song's base tone or falls back to the song filename. */
    async function installTaSwitcherForSong(songFile, toneChanges, toneBase) {
        const cfg = readTaStore();
        if (!cfg.enabled) return false;
        const switcher = new ToneAutomationSwitcher();
        window._toneSwitcher = switcher;
        const initialInput = String(toneBase || songFile || '').trim();
        if (initialInput) await switcher.switchToTone(initialInput);
        return true;
    }

    function renderToneAutomationFilters() {
        const container = $('ae-ta-filters');
        if (!container) return;
        const cfg = readTaStore();
        container.innerHTML = '';
        for (const cat of TA_PRECEDENCE) {
            const defaults = TA_DEFAULT_KEYWORDS[cat] || [];
            const custom = parseTaKeywordList(cfg.customKeywords?.[cat]).join(', ');
            const wrap = document.createElement('div');
            wrap.className = 'rounded bg-slate-800/40 px-2 py-1.5';
            wrap.innerHTML = `
                <div class="flex items-baseline gap-2 mb-1">
                    <span class="text-xs font-medium text-slate-300 w-16 shrink-0">${escHtml(TA_CATEGORY_LABELS[cat])}</span>
                    <span class="text-[11px] text-slate-500 truncate" title="${escHtml(defaults.join(', '))}">defaults: ${escHtml(defaults.join(', '))}</span>
                </div>
                <input type="text"
                    data-ta-custom="${escHtml(cat)}"
                    placeholder="Custom keywords (comma-separated)"
                    value="${escHtml(custom)}"
                    class="w-full bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-200">
            `;
            container.appendChild(wrap);
        }
        container.querySelectorAll('input[data-ta-custom]').forEach(inp => {
            inp.addEventListener('change', () => {
                const cur = readTaStore();
                cur.customKeywords = cur.customKeywords || {};
                cur.customKeywords[inp.dataset.taCustom] = inp.value;
                writeTaStore(cur);
            });
        });
    }

    function renderToneAutomationTargets() {
        const container = $('ae-ta-targets');
        if (!container) return;
        const cfg = readTaStore();
        const presetNames = Object.keys(getPresets());
        container.innerHTML = '';
        for (const row of TA_TARGET_ROWS) {
            const current = cfg.targets?.[row.key] || '';
            const div = document.createElement('div');
            div.className = 'flex items-center gap-2';
            div.innerHTML = `
                <span class="text-xs text-slate-400 w-32 shrink-0">${escHtml(row.label)}</span>
                <select data-ta-target="${escHtml(row.key)}" class="flex-1 bg-slate-700 border border-slate-600 rounded px-2 py-1 text-xs text-slate-200">
                    <option value="">-- none --</option>
                    ${presetNames.map(n => `<option value="${escHtml(n)}" ${current === n ? 'selected' : ''}>${escHtml(n)}</option>`).join('')}
                </select>
            `;
            container.appendChild(div);
        }
        container.querySelectorAll('select[data-ta-target]').forEach(sel => {
            sel.addEventListener('change', () => {
                const cur = readTaStore();
                cur.targets = cur.targets || {};
                if (sel.value) cur.targets[sel.dataset.taTarget] = sel.value;
                else delete cur.targets[sel.dataset.taTarget];
                writeTaStore(cur);
            });
        });
    }

    function renderToneAutomationSettings() {
        renderToneAutomationFilters();
        renderToneAutomationTargets();
    }

    async function activateToneAutomationForCurrentSong() {
        const songKey = getCurrentSongKey();
        const hw = window.highway || window._slopsmithHighway;
        const toneChanges = hw?.getToneChanges ? hw.getToneChanges() : [];
        const toneBase = hw?.getToneBase ? hw.getToneBase() : '';
        await installTaSwitcherForSong(songKey, toneChanges, toneBase);
    }

    async function deactivateToneAutomation() {
        window._toneSwitcher = null;
        const songKey = getCurrentSongKey();
        // Try to restore any manual tone mapping the user had configured first;
        // if there isn't one, fall back to the global default preset.
        // Check before the call — applyToneMappingsNow returns undefined regardless
        // of whether it applied a mapping, so we can't infer success after the fact.
        const hasMappings = Object.keys(getToneMappings(songKey)).length > 0;
        try { await applyToneMappingsNow(songKey, { force: true }); } catch (e) { /* ignore */ }
        if (!hasMappings) {
            await loadDefaultPreset('tone-automation-off');
        }
    }

    function setupToneAutomationSettingsEvents() {
        renderToneAutomationSettings();
    }

    /** Public hooks consumed by the playSong wrapper in IIFE 2 below and by the
     *  Chain modal toggle. */
    window._aeToneAutomation = {
        isEnabled: () => readTaStore().enabled,
        getConfig: readTaStore,
        getSessionEffectivePreset: getTaSessionEffectivePreset,
        clearSessionOverrides() {
            window._aeTaSessionOverrides = {};
        },
        setEnabled(value) {
            const cur = readTaStore();
            cur.enabled = !!value;
            writeTaStore(cur);
        },
        classify: classifyTaCategory,
        resolvePreset: resolveTaPreset,
        applyFor: applyToneAutomationFor,
        installSwitcherForSong: installTaSwitcherForSong,
        renderTargets: renderToneAutomationTargets,
        renderSettings: renderToneAutomationSettings,
        ToneAutomationSwitcher,
        TA_DEFAULT_KEYWORDS,
        TA_CATEGORY_LABELS,
        TA_TARGET_ROWS,
    };

    // ── Start ─────────────────────────────────────────────────────────────────
    init().then(() => {
        renderPresetList();
        renderToneAutomationSettings();
    }).catch(e => console.error('[audio-engine] init error:', e));

    if (window.slopsmith?.on) {
        let _reapplyDebounceTimer = null;
        let _reapplyFollowupTimer = null;
        const scheduleReapply = () => {
            window._toneMappingsDirty = true;
            const songKey = getCurrentSongKey();
            if (_reapplyDebounceTimer) clearTimeout(_reapplyDebounceTimer);
            if (_reapplyFollowupTimer) clearTimeout(_reapplyFollowupTimer);
            _reapplyDebounceTimer = setTimeout(() => {
                void applyToneMappingsNow(songKey).catch(e => console.warn('[tone-switcher] Reapply (debounce) failed:', e));
                void refreshTonePanelIfOpen();
            }, 600);
            _reapplyFollowupTimer = setTimeout(() => {
                void applyToneMappingsNow(songKey).catch(e => console.warn('[tone-switcher] Reapply (followup) failed:', e));
                void refreshTonePanelIfOpen();
            }, 1500);
        };
        window.slopsmith.on('arrangement:changed', scheduleReapply);
        window.slopsmith.on('song:ready', scheduleReapply);
    }
})();

// ── Chain button + tone auto-switch (runs outside IIFE so it works without audio API) ──
(function() {
    // Hook registry shared across re-evaluations; see the IIFE 1 comment for
    // why we don't preemptively clear toneAutoMonitor here.
    const hookState = window.__slopsmithDesktopAudioHooks;
    const origPS = hookState.toneAutoBasePlaySong || window.playSong;
    if (!origPS) return;

    let _lastTone = null;
    // Throttle the "_toneSwitcher not ready" warning — the tone monitor polls
    // at 50ms, so without this it would log every tick while the switcher is
    // unavailable. Logged once per not-ready episode; reset on a successful switch.
    let _toneSwitcherWarned = false;
    /** Song + arrangement — invalidates preload when switching Lead/Bass/etc. on the same file */
    let _preloadedToneCacheKey = null;

    // Reuse helpers from the audio-API IIFE — avoids duplicate implementations drifting apart.
    const normalizeSongKey = window._aeNormalizeSongKey || ((raw) => String(raw || '').replace(/\\/g, '/').trim());
    const getCurrentSongKey = window._aeGetCurrentSongKey || (() => normalizeSongKey(window._currentSongFile || window._slopsmithSongKey || ''));
    const getToneChangeTime = window._aeGetToneChangeTime || ((tc) => { const t = tc?.t ?? tc?.time ?? tc?.timestamp ?? tc?.at; return Number.isFinite(t) ? t : Infinity; });

    function getTonePreloadCacheKey() {
        const sk = getCurrentSongKey();
        const arr = String(window.slopsmith?.currentSong?.arrangement || '').trim().toLowerCase();
        return `${sk}::${arr}`;
    }

    function showToneToast(name) {
        let toast = document.getElementById('tone-toast');
        if (!toast) {
            toast = document.createElement('div');
            toast.id = 'tone-toast';
            toast.style.cssText = 'position:fixed;top:60px;right:20px;z-index:9999;padding:8px 16px;border-radius:8px;background:rgba(234,88,12,0.9);color:white;font-size:13px;font-weight:600;pointer-events:none;transition:opacity 0.5s;opacity:0;';
            document.body.appendChild(toast);
        }
        toast.textContent = 'Tone: ' + name;
        toast.style.opacity = '1';
        clearTimeout(toast._timer);
        toast._timer = setTimeout(() => { toast.style.opacity = '0'; }, 2000);
    }

    // Delegate to the audio-API IIFE's implementation — avoids duplicate gain/UI logic drifting.
    // The _api parameter is accepted for call-site compatibility but ignored when delegating;
    // the first IIFE's version uses its own closure-scoped api (same underlying native object).
    function applyPresetGainLevels(_api, preset) {
        if (window._aeApplyPresetGainLevels) { window._aeApplyPresetGainLevels(preset); return; }
        // Fallback when first IIFE hasn't loaded yet (should not happen in normal flow).
        const inputLin = Number.isFinite(Number(preset?.inputGain)) ? Number(preset.inputGain) : 1;
        const outputLin = Number.isFinite(Number(preset?.outputGain)) ? Number(preset.outputGain) : 1;
        // Output gain → 'chain' (guitar-only) so a preset switch doesn't move
        // the song volume — consistent with the first IIFE's applyPresetGainLevels.
        if (_api?.setGain) { _api.setGain('input', inputLin); _api.setGain('chain', outputLin); }
    }

    function applyPresetNoiseGate(_api, preset) {
        if (window._aeApplyPresetNoiseGate) {
            window._aeApplyPresetNoiseGate(preset);
            return;
        }
        void _api;
        void preset;
    }

    window._aeStartToneAutoSwitch = function() { startToneAutoSwitch(); };
    window._aeStopToneMonitor = function() {
        // Public teardown used by _applyToneMappingsImpl; clear both monitors so
        // the IIFE 1 toneSwitcher can't keep applying stale mappings either.
        if (hookState.toneMonitorInterval) { clearInterval(hookState.toneMonitorInterval); hookState.toneMonitorInterval = null; }
        if (hookState.toneAutoMonitor) { clearInterval(hookState.toneAutoMonitor); hookState.toneAutoMonitor = null; }
        window._toneAutoSwitchActive = false;
    };

    function startToneAutoSwitch() {
        if (hookState.toneAutoMonitor) clearInterval(hookState.toneAutoMonitor);
        _lastTone = null;
        _toneSwitcherWarned = false;  // fresh not-ready warning per monitor session
        window._toneAutoSwitchActive = true;

        hookState.toneAutoMonitor = setInterval(() => {
            const hw = window.highway || window._slopsmithHighway;
            if (!hw || !hw.getTime) return;

            const autoOn = localStorage.getItem('slopsmith-tone-auto-switch') === 'true';
            const taOn = window._aeToneAutomation?.isEnabled?.() === true;
            if (!autoOn && !taOn) {
                clearInterval(hookState.toneAutoMonitor);
                hookState.toneAutoMonitor = null;
                window._toneAutoSwitchActive = false;
                return;
            }

            const t = hw.getTime();
            const changes = hw.getToneChanges ? hw.getToneChanges() : [];
            const base = hw.getToneBase ? hw.getToneBase() : '';
            if (changes.length === 0) return;

            let activeTone = String(base || '').trim();
            for (const tc of changes) {
                if (getToneChangeTime(tc) <= t) {
                    const n = String(tc?.name || '').trim();
                    if (n) activeTone = n;
                } else break;
            }

            if (activeTone && activeTone !== _lastTone) {
                // Only mark the tone consumed once it is actually applied.
                // Updating _lastTone before the _toneSwitcher null-check would
                // permanently drop a switch that arrives during the startup
                // window before _toneSwitcher is installed — the next 50 ms
                // poll would see activeTone === _lastTone and skip it.
                if (window._toneSwitcher) {
                    _lastTone = activeTone;
                    showToneToast(activeTone);
                    window._toneSwitcher.switchToTone(activeTone);
                    _toneSwitcherWarned = false;
                } else if (!_toneSwitcherWarned) {
                    // Throttled: log once, not every 50ms poll.
                    _toneSwitcherWarned = true;
                    console.log('[tone-switcher] _toneSwitcher not ready — retrying until installed');
                }
            }
        }, 50);
    }

    hookState.toneAutoImpl = async function(filename, arrangement, nextPlaySong) {
        if (window._aeMarkSongTransition) window._aeMarkSongTransition(7000);
        if (hookState.toneAutoMonitor) { clearInterval(hookState.toneAutoMonitor); hookState.toneAutoMonitor = null; }
        window._toneAutoSwitchActive = false;
        window._aeDidClearChainForNewSong = false;
        window._aeClearingChainForNewSong = false;
        _lastTone = null;
        _toneSwitcherWarned = false;
        window._aeTaSessionOverrides = {};
        if (window._closeChainPanel) window._closeChainPanel();
        window._currentSongFile = decodeURIComponent(filename);
        window._slopsmithSongKey = normalizeSongKey(window._currentSongFile);
        // Reset preload tracking when the song file changes (not when only arrangement/track changes)
        const skNow = getCurrentSongKey();
        if (_preloadedToneCacheKey) {
            const cachedSongKey = _preloadedToneCacheKey.split('::')[0];
            if (cachedSongKey !== skNow) {
                _preloadedToneCacheKey = null;
                window._toneSwitcher = null;
            }
        }

        await nextPlaySong.call(this, filename, arrangement);

        // Tear down the menu/default chain after load (never await clearChain here — it can re-enter
        // the audio host during playSong and crash). The timed preload below rebuilds song presets.
        setTimeout(() => {
            if (window._aeClearChainForNewSong) {
                window._aeClearingChainForNewSong = true;
                void window._aeClearChainForNewSong().then(() => {
                    window._aeDidClearChainForNewSong = true;
                }).catch((e) => {
                    console.warn('[audio-engine] clearChainForNewSong failed:', e);
                }).finally(() => {
                    window._aeClearingChainForNewSong = false;
                });
            }
        }, 400);

        // Inject Chain button
        setTimeout(() => {
            const controls = document.getElementById('player-controls');
            if (!controls || document.getElementById('btn-chain-switch')) return;
            const closeBtn = controls.querySelector('button[onclick*="showScreen"]');
            if (closeBtn && !closeBtn.dataset.chainPanelCloseBound) {
                closeBtn.addEventListener('click', () => {
                    if (window._closeChainPanel) window._closeChainPanel();
                    if (window._aeLoadDefaultPreset) void window._aeLoadDefaultPreset('player-exit');
                }, { capture: true });
                closeBtn.dataset.chainPanelCloseBound = '1';
            }
            const btn = document.createElement('button');
            btn.id = 'btn-chain-switch';
            btn.className = 'px-3 py-1.5 bg-orange-900/40 hover:bg-orange-900/60 rounded-lg text-xs text-orange-300 transition';
            btn.textContent = 'Chain';
            btn.onclick = () => window._toggleChainPanel && window._toggleChainPanel();
            if (closeBtn) controls.insertBefore(btn, closeBtn);
            else controls.appendChild(btn);
        }, 500);

        // Start tone monitoring and preload presets after WebSocket delivers tone data
        setTimeout(async () => {
            try {
            // Only start the 50ms polling interval when at least one switching mode is on;
            // starting it unconditionally wastes cycles on localStorage + highway reads every
            // 50ms for songs where both auto-switch and Tone Automation are disabled.
            const autoOn = localStorage.getItem('slopsmith-tone-auto-switch') === 'true';
            const taOn = window._aeToneAutomation?.isEnabled?.() === true;
            if (autoOn || taOn) {
                startToneAutoSwitch();
            } else {
                if (hookState.toneAutoMonitor) { clearInterval(hookState.toneAutoMonitor); hookState.toneAutoMonitor = null; }
                window._toneAutoSwitchActive = false;
            }

            // Preload presets for tone switching
            const api = window.slopsmithDesktop?.audio;
            const hw = window.highway || window._slopsmithHighway;
            if (!api || !hw) return;

            const songKeyPreflight = getCurrentSongKey();
            let midiPreflight = null;
            try {
                const rawMap = JSON.parse(localStorage.getItem('slopsmith-tone-mappings') || '{}') || {};
                midiPreflight = rawMap.midiPC?.[songKeyPreflight];
            } catch (e) { /* ignore */ }
            // MIDI PC mode talks to an existing VST slot — do not wipe the chain here (outer MIDI block
            // does not reload processors). Bypass / Tone Automation need a clean slate vs menu default.
            // Also skip when the 400ms clearChainForNewSong is in-flight or already completed —
            // _aeClearingChainForNewSong is set the moment the async clear begins; _aeDidClearChainForNewSong
            // is set on resolution. Checking both prevents a second clearChain racing with a slow first one,
            // which could crash some JUCE bridges. preloadForSong calls clearChain itself anyway.
            const skipPreflightClear = (midiPreflight?.mode === 'midi' && Number(midiPreflight.vstSlotId) >= 0)
                || !!window._aeDidClearChainForNewSong
                || !!window._aeClearingChainForNewSong;
            // Track whether the chain has been cleared by any path so the bypass preload
            // below can skip its own clearChain and avoid a redundant second IPC call.
            let chainClearedForLoad = skipPreflightClear;
            if (!skipPreflightClear) {
                try {
                    await api.clearChain();
                    chainClearedForLoad = true;
                    try {
                        localStorage.setItem('slopsmith-signal-chain', '[]');
                    } catch (e) { /* ignore */ }
                } catch (e) {
                    console.warn('[tone-switcher] preflight clearChain:', e);
                }
            }

            // Wait briefly for highway tone data to arrive (ws may still be connecting).
            let toneChanges = hw.getToneChanges ? hw.getToneChanges() : [];
            let toneBase = hw.getToneBase ? hw.getToneBase() : '';
            for (let attempt = 0; attempt < 6 && toneChanges.length === 0 && !toneBase; attempt++) {
                await new Promise(r => setTimeout(r, 500));
                toneChanges = hw.getToneChanges ? hw.getToneChanges() : [];
                toneBase = hw.getToneBase ? hw.getToneBase() : '';
            }

            const songKey = getCurrentSongKey();

            // Check for MIDI PC mode
            let allMappingsData = {};
            try { allMappingsData = JSON.parse(localStorage.getItem('slopsmith-tone-mappings') || '{}') || {}; }
            catch (e) { allMappingsData = {}; }
            const midiConfig = allMappingsData.midiPC?.[songKey];
            const wantsMidi = midiConfig?.mode === 'midi';

            // Invalidate the preload cache when mappings changed externally (MIDI save, preset change, etc.)
            if (window._toneMappingsDirty) { _preloadedToneCacheKey = null; window._toneMappingsDirty = false; }

            // Skip if already preloaded for this song+arrangement with the same mode AND timeline data is present.
            // Songs without tone_changes have a no-op switchToTone, so re-entry must force re-apply.
            const cacheKeyNow = getTonePreloadCacheKey();
            const hasTimelineForCacheCheck = (toneChanges?.length || 0) > 0 || !!toneBase;
            if (_preloadedToneCacheKey === cacheKeyNow && window._toneSwitcher && hasTimelineForCacheCheck) {
                const currentIsMidi = !!window._toneSwitcher.midiMode;
                if (currentIsMidi === wantsMidi) {
                    const tbTrim = String(toneBase || '').trim();
                    const tcArr = Array.isArray(toneChanges) ? toneChanges : [];
                    let effBase = tbTrim;
                    if (!effBase && tcArr.length > 0) {
                        const sorted = [...tcArr].filter(tc => String(tc?.name || '').trim())
                            .sort((a, b) => getToneChangeTime(a) - getToneChangeTime(b));
                        if (sorted.length) effBase = String(sorted[0].name).trim();
                    }
                    const switchKey = effBase || tbTrim;
                    window._toneSwitcher.switchToTone(switchKey);
                    const tpm = window._toneSwitcher.tonePresetMap;
                    const p = tpm && switchKey ? tpm[switchKey] : null;
                    if (p) {
                        applyPresetGainLevels(api, p);
                        applyPresetNoiseGate(api, p);
                    }
                    return;
                }
                // Mode changed — reset and re-preload
                _preloadedToneCacheKey = null;
                window._toneSwitcher = null;
            }
            if (_preloadedToneCacheKey === cacheKeyNow && !hasTimelineForCacheCheck) {
                _preloadedToneCacheKey = null;
                window._toneSwitcher = null;
            }

            // Tone Automation overrides both MIDI and bypass modes when enabled.
            // It installs a classifier-driven `_toneSwitcher`; the auto-switch
            // monitor above invokes it on every tone change.
            if (window._aeToneAutomation?.isEnabled?.()) {
                const installed = await window._aeToneAutomation.installSwitcherForSong(
                    window._currentSongFile || filename, toneChanges, toneBase
                );
                if (installed) {
                    _preloadedToneCacheKey = getTonePreloadCacheKey();
                    startToneAutoSwitch();
                    console.log('[tone-automation] installed for song:', window._currentSongFile || filename);
                    return;
                }
            }

            console.log('[tone-switcher] Mode:', wantsMidi ? 'MIDI' : 'bypass', 'config:', JSON.stringify(midiConfig));

            if (midiConfig?.mode === 'midi' && midiConfig.vstSlotId >= 0) {
                // MIDI PC mode — send program changes to a single VST
                const midiMappings = midiConfig.mappings || {};
                window._toneSwitcher = {
                    activeTone: null,
                    midiMode: true,
                    switchToTone(name) {
                        console.log('[tone-switcher] switchToTone called:', name, 'current:', this.activeTone, 'midiMode:', this.midiMode);
                        if (name === this.activeTone) return;
                        const program = midiMappings[name];
                        const _api = window.slopsmithDesktop?.audio;
                        console.log('[tone-switcher] program:', program, 'api:', !!_api, 'sendMidi:', !!_api?.sendMidiToSlot, 'slotId:', midiConfig.vstSlotId);
                        if (program !== undefined && _api?.sendMidiToSlot) {
                            _api.sendMidiToSlot(midiConfig.vstSlotId, 0, midiConfig.channel || 1, program);
                            console.log('[tone-switcher] MIDI PC SENT:', name, '-> program', program);
                        }
                        this.activeTone = name;
                    }
                };
                // Send initial PC for base tone
                const _apiInit = window.slopsmithDesktop?.audio;
                if (midiMappings[toneBase] !== undefined && _apiInit?.sendMidiToSlot) {
                    _apiInit.sendMidiToSlot(midiConfig.vstSlotId, 0, midiConfig.channel || 1, midiMappings[toneBase]);
                }
                _preloadedToneCacheKey = getTonePreloadCacheKey();
                console.log('[tone-switcher] MIDI PC mode for:', Object.keys(midiMappings));
            } else {
                // Bypass-toggle mode — preload all presets
                const mappings = { ...(allMappingsData.global || {}), ...(allMappingsData.songs?.[songKey] || {}) };
                if (Object.keys(mappings).length === 0) return;

                const presets = window._aeGetPresets ? window._aeGetPresets() : {};
                const hasTimelineToneData = toneChanges.length > 0 || !!toneBase;
                if (!hasTimelineToneData) {
                    const toneNames = await window._aeGetOriginalToneNamesForCurrentArrangement?.(songKey) ?? [];
                    const lookup = (name) => (window._aeResolveTonePresetName
                        ? window._aeResolveTonePresetName(mappings, name)
                        : ((window._aeFindMappingForTone ? window._aeFindMappingForTone(mappings, name) : mappings[name]) || mappings['$default']));
                    const matchedTone = toneNames.find(n => !!lookup(n));
                    const selectedTone = matchedTone || '$default';
                    const firstNonDefaultKey = Object.keys(mappings).filter(k => k !== '$default')[0];
                    const presetName = lookup(selectedTone) || (firstNonDefaultKey ? mappings[firstNonDefaultKey] : null);
                    const preset = presetName ? presets[presetName] : null;
                    if (preset?.nativePreset) {
                        if (await window._aeReplaceChainWithPresetBlob?.(preset, 'preload-no-timeline', { snapshot: false })) {
                            console.log('[tone-switcher] Loaded mapped preset (no tone_changes):', selectedTone, '->', presetName);
                        }
                    } else if (window._aeLoadDefaultPreset) {
                        await window._aeLoadDefaultPreset('tone-none');
                    }
                    window._toneSwitcher = {
                        activeTone: selectedTone,
                        toneSlotMap: {},
                        switchToTone() { /* no timeline tones available for song */ }
                    };
                    _preloadedToneCacheKey = getTonePreloadCacheKey();
                    return;
                }

                const rawTc = Array.isArray(toneChanges) ? toneChanges : [];
                const tbTrim = String(toneBase || '').trim();
                const toneNameSet = new Set();
                if (tbTrim) toneNameSet.add(tbTrim);
                for (const tc of rawTc) {
                    const n = String(tc?.name || '').trim();
                    if (n) toneNameSet.add(n);
                }
                let effectiveBase = tbTrim;
                if (!effectiveBase && rawTc.length > 0) {
                    const sorted = [...rawTc]
                        .filter(tc => String(tc?.name || '').trim())
                        .sort((a, b) => getToneChangeTime(a) - getToneChangeTime(b));
                    if (sorted.length) effectiveBase = String(sorted[0].name).trim();
                }
                if (!effectiveBase && toneNameSet.size > 0) {
                    effectiveBase = toneNameSet.values().next().value;
                }

                if (toneNameSet.size === 0) {
                    console.warn('[tone-switcher] Bypass preload: no valid tone names');
                    _preloadedToneCacheKey = getTonePreloadCacheKey();
                    return;
                }

                if (!chainClearedForLoad) {
                    await api.clearChain();
                    chainClearedForLoad = true;
                }
                window._toneSwitcher = null;
                const toneSlotMap = {};
                const tonePresetMap = {};

                const lookupBypass = (name) => (window._aeResolveTonePresetName
                    ? window._aeResolveTonePresetName(mappings, name)
                    : ((window._aeFindMappingForTone ? window._aeFindMappingForTone(mappings, name) : mappings[name]) || mappings['$default']));
                for (const toneName of toneNameSet) {
                    const presetName = lookupBypass(toneName);
                    if (!presetName || !presets[presetName]) continue;
                    const preset = presets[presetName];
                    const slotIds = [];
                    const chainItems = Array.isArray(preset?.items) ? preset.items : [];
                    for (const item of chainItems) {
                        let slotId = -1;
                        if (item.type === 'NAM' && item.path) slotId = await api.loadNAMModel(item.path);
                        else if (item.type === 'IR' && item.path) slotId = await api.loadIR(item.path);
                        else if (item.type === 'VST' && item.path) slotId = await api.loadVST(item.path);
                        if (slotId >= 0) slotIds.push(slotId);
                    }
                    toneSlotMap[toneName] = slotIds;
                    tonePresetMap[toneName] = preset;
                    if (toneName !== effectiveBase && slotIds.length > 0) {
                        await api.setMultiBypass(slotIds.map(id => ({ slotId: id, bypassed: true })));
                    }
                }

                const initialPreset = tonePresetMap[effectiveBase];
                if (initialPreset) {
                    applyPresetGainLevels(api, initialPreset);
                    applyPresetNoiseGate(api, initialPreset);
                }

                window._toneSwitcher = {
                    activeTone: effectiveBase,
                    toneSlotMap,
                    tonePresetMap,
                    switchToTone(name) {
                        if (name === this.activeTone) return;
                        if (!this.toneSlotMap[name]) return;
                        const bypassList = [];
                        if (this.activeTone && this.toneSlotMap[this.activeTone]) {
                            for (const id of this.toneSlotMap[this.activeTone]) bypassList.push({ slotId: id, bypassed: true });
                        }
                        for (const id of this.toneSlotMap[name]) bypassList.push({ slotId: id, bypassed: false });
                        if (bypassList.length > 0) api.setMultiBypass(bypassList);
                        this.activeTone = name;
                        const newPreset = this.tonePresetMap?.[name];
                        if (newPreset) {
                            applyPresetGainLevels(api, newPreset);
                            applyPresetNoiseGate(api, newPreset);
                        }
                        console.log('[tone-switcher] Switched to:', name);
                    }
                };
                _preloadedToneCacheKey = getTonePreloadCacheKey();
                console.log('[tone-switcher] Bypass mode preloaded:', Object.keys(toneSlotMap));
                if (autoOn) startToneAutoSwitch();
            }
            } catch (err) {
                console.error('[tone-switcher] Preload failed:', err);
            }
        }, 800);
    };

    if (!hookState.toneAutoInstalled) {
        hookState.toneAutoBasePlaySong = origPS;
        window.playSong = async function(filename, arrangement) {
            return hookState.toneAutoImpl.call(this, filename, arrangement, hookState.toneAutoBasePlaySong);
        };
        hookState.toneAutoInstalled = true;
    }

    hookState.stopSongImpl = async function(args, nextStopSong) {
        // Tear down both monitors (IIFE 1's toneMonitorInterval + IIFE 2's
        // toneAutoMonitor) — leaving either running after stopSong would keep
        // polling/switching tones on a stopped song until the next playSong.
        if (window._aeStopToneMonitor) window._aeStopToneMonitor();
        try {
            return await nextStopSong.apply(this, args);
        } finally {
            if (window._closeChainPanel) window._closeChainPanel();
            if (window._aeLoadDefaultPreset) void window._aeLoadDefaultPreset('song-stop');
        }
    };
    if (typeof window.stopSong === 'function' && !hookState.stopSongInstalled) {
        hookState.stopSongBaseStopSong = window.stopSong;
        window.stopSong = async function(...args) {
            return hookState.stopSongImpl.call(this, args, hookState.stopSongBaseStopSong);
        };
        hookState.stopSongInstalled = true;
    }

    // Inject Chain button immediately at startup so it's always visible in the
    // player controls — don't wait for the first song play.
    function tryInjectChainButton() {
        const controls = document.getElementById('player-controls');
        if (!controls || document.getElementById('btn-chain-switch')) return;
        const closeBtn = controls.querySelector('button[onclick*="showScreen"]');
        if (closeBtn && !closeBtn.dataset.chainPanelCloseBound) {
            closeBtn.addEventListener('click', () => {
                if (window._closeChainPanel) window._closeChainPanel();
                if (window._aeLoadDefaultPreset) void window._aeLoadDefaultPreset('player-exit');
            }, { capture: true });
            closeBtn.dataset.chainPanelCloseBound = '1';
        }
        const btn = document.createElement('button');
        btn.id = 'btn-chain-switch';
        btn.className = 'px-3 py-1.5 bg-orange-900/40 hover:bg-orange-900/60 rounded-lg text-xs text-orange-300 transition';
        btn.textContent = 'Chain';
        btn.onclick = () => window._toggleChainPanel && window._toggleChainPanel();
        if (closeBtn) controls.insertBefore(btn, closeBtn);
        else controls.appendChild(btn);
    }
    // Allow app.js to finish initialising before querying the DOM.
    setTimeout(tryInjectChainButton, 0);
})();
