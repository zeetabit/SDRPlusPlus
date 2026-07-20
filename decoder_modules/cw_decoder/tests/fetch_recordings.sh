#!/usr/bin/env bash
#
# Fetch ARRL W1AW code-practice recordings for the [recording] test gate.
#
# These are real transmissions published with their exact text, which is what
# makes them usable as ground truth — the synthetic generator can only ever
# validate the decoder against its own assumptions (docs §15).
#
# The audio is ARRL's and the text derives from QST; neither is committed.
#
# Sessions are pinned by date. ARRL rotates the archive listings, so a pinned
# file can 404 in the future; the script reports which ones failed rather than
# leaving a half-populated cache that scores against missing material.

set -u

DEST="${CW_RECORDINGS_DIR:-$(cd "$(dirname "$0")" && pwd)/recordings}"
BASE="http://www.arrl.org/files/file/Morse/Archive"

# speed:session — the exact files the CER thresholds in test_recording.cpp
# were measured against. Changing a session means re-measuring the gate.
SESSIONS=(
    "5:260204_05"
    "10:260204_10"
    "15:260218_15"
    "20:260203_20"
    "35:260203_35"
)

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg is required to decode ARRL MP3s to WAV." >&2
    echo "       macOS: brew install ffmpeg" >&2
    exit 1
fi

mkdir -p "$DEST"
echo "Fetching ARRL W1AW code practice into: $DEST"

failed=0
for entry in "${SESSIONS[@]}"; do
    wpm="${entry%%:*}"
    stem="${entry##*:}"
    wav="$DEST/w1aw_${wpm}wpm.wav"
    txt="$DEST/w1aw_${wpm}wpm.txt"

    if [ -s "$wav" ] && [ -s "$txt" ]; then
        echo "  ${wpm} WPM: cached"
        continue
    fi

    # Staged through temporaries so a failed fetch leaves no partial pair.
    # The .mp3 suffix is kept because ffmpeg infers the input format from it.
    mp3="$DEST/.${stem}.tmp.mp3"
    txtTmp="$DEST/.${stem}.txt.part"

    if ! curl -fsSL -m 180 -o "$mp3" "${BASE}/${wpm}%20WPM/${stem}WPM.mp3"; then
        echo "  ${wpm} WPM: FAILED (audio ${stem}WPM.mp3)" >&2
        rm -f "$mp3"; failed=$((failed + 1)); continue
    fi
    if ! curl -fsSL -m 60 -o "$txtTmp" "${BASE}/${wpm}%20WPM/${stem}.txt"; then
        echo "  ${wpm} WPM: FAILED (text ${stem}.txt)" >&2
        rm -f "$mp3" "$txtTmp"; failed=$((failed + 1)); continue
    fi

    # 8 kHz mono matches the decoder's internal IQ rate, so the test resamples
    # nothing and the measured group delay is the chain's own.
    if ! ffmpeg -y -loglevel error -i "$mp3" -ac 1 -ar 8000 -c:a pcm_s16le -f wav "$wav.part"; then
        echo "  ${wpm} WPM: FAILED (ffmpeg decode)" >&2
        rm -f "$mp3" "$txtTmp" "$wav.part"; failed=$((failed + 1)); continue
    fi

    mv "$wav.part" "$wav"
    mv "$txtTmp" "$txt"
    rm -f "$mp3"
    echo "  ${wpm} WPM: ok ($(du -h "$wav" | cut -f1))"
done

if [ "$failed" -gt 0 ]; then
    echo ""
    echo "$failed session(s) failed. The archive listings rotate; find current" >&2
    echo "sessions at http://www.arrl.org/code-practice-files and update" >&2
    echo "SESSIONS above, then re-measure the thresholds in test_recording.cpp." >&2
    exit 1
fi

echo "Done. Run: ./cw_decoder_tests \"[recording]\""
