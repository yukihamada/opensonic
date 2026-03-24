"""
IMA-ADPCM codec matching the C++ and Swift implementations.
"""

from __future__ import annotations

import array

# Standard IMA-ADPCM step size table (89 entries).
STEP_TABLE: list[int] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]

# Standard IMA-ADPCM index adjustment table.
INDEX_TABLE: list[int] = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]


class ADPCMState:
    __slots__ = ("predicted", "step_index")

    def __init__(self, predicted: int = 0, step_index: int = 0) -> None:
        self.predicted = predicted
        self.step_index = min(step_index, 88)


def decode_adpcm_payload(payload: bytes | bytearray) -> array.array[int] | None:
    """Decode an ADPCM payload with 4-byte header to int16 PCM samples.

    Header:
    - [0-1]: predictor (int16 LE)
    - [2]: step index
    - [3]: reserved

    Returns array of int16 samples, or None on error.
    """
    if len(payload) < 4:
        return None

    # Parse header — sign-extend int16
    predicted = int.from_bytes(payload[0:2], "little", signed=True)
    step_index = min(payload[2], 88)
    state = ADPCMState(predicted, step_index)

    return decode_adpcm(payload[4:], state)


def decode_adpcm(adpcm: bytes | bytearray, state: ADPCMState) -> array.array[int]:
    """Decode raw ADPCM nibbles to int16 PCM samples.
    Each byte contains 2 nibbles (low nibble first).
    """
    n_samples = len(adpcm) * 2
    out = array.array("h", [0] * n_samples)  # signed short
    out_idx = 0

    for byte in adpcm:
        for nibble_idx in range(2):
            code = (byte & 0x0F) if nibble_idx == 0 else (byte >> 4)
            step = STEP_TABLE[state.step_index]

            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            if code & 8:
                delta = -delta

            new_predicted = max(-32768, min(32767, state.predicted + delta))
            state.predicted = new_predicted

            new_idx = max(0, min(88, state.step_index + INDEX_TABLE[code]))
            state.step_index = new_idx

            out[out_idx] = state.predicted
            out_idx += 1

    return out
