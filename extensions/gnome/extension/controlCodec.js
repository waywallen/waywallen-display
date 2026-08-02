const FRAME_TYPES = new Set([
    'connection',
    'presentation-snapshot',
    'presentation-state',
    'reset',
]);

export const PauseEffectKind = Object.freeze({
    NONE: 0,
    BLUR: 1,
});

function validGeneration(value, allowZero = false) {
    return Number.isSafeInteger(value) && (allowZero ? value >= 0 : value > 0);
}

function validateGeometry(geometry) {
    if (!geometry || !['x', 'y', 'width', 'height'].every(k => Number.isInteger(geometry[k])))
        throw new Error('invalid monitor geometry');
    if (geometry.width <= 0 || geometry.height <= 0)
        throw new Error('invalid monitor extent');
}

function validateState(config) {
    if (!config || !validGeneration(config.generation) ||
        !validGeneration(config.configGeneration) ||
        typeof config.pauseEffect?.active !== 'boolean')
        throw new Error('invalid presentation state');
}

function validateSnapshot(snapshot) {
    if (!snapshot || !validGeneration(snapshot.config?.generation) ||
        !Object.values(PauseEffectKind).includes(snapshot.config?.pauseEffect?.kind) ||
        !Number.isInteger(snapshot.config?.pauseEffect?.blur?.radius) ||
        snapshot.config.pauseEffect.blur.radius < 1 ||
        snapshot.config.pauseEffect.blur.radius > 64)
        throw new Error('invalid presentation snapshot');
    validateState(snapshot.state);
    if (snapshot.state.configGeneration !== snapshot.config.generation ||
        (snapshot.config.pauseEffect.kind === PauseEffectKind.NONE &&
         snapshot.state.pauseEffect.active))
        throw new Error('inconsistent presentation snapshot');
}

export function validateControlFrame(frame) {
    if (!frame || !FRAME_TYPES.has(frame.type))
        throw new Error('unknown control frame');
    validateGeometry(frame.geometry);
    switch (frame.type) {
    case 'connection':
    case 'presentation-snapshot':
        validateSnapshot(frame.presentation);
        break;
    case 'presentation-state':
        validateState(frame.state);
        break;
    case 'reset':
        break;
    }
    return frame;
}

export function encodeControlFrame(frame) {
    return `${JSON.stringify(validateControlFrame(frame))}\n`;
}

export function decodeControlFrame(line) {
    return validateControlFrame(JSON.parse(line));
}
