#!/bin/bash
# Pre-synthesis hook: warn about NUM_TAPS and timing before launching a long build
# Reads the image core YAML and reports current config to Claude

ICORE_YAML="/home/wines/Desktop/OpenAirLink/rfnoc-openairlink/icores/x410_rfnoc_image_core_4chan_sparse.yml"

if [ -f "$ICORE_YAML" ]; then
    NUM_TAPS=$(grep -m1 "NUM_TAPS:" "$ICORE_YAML" | awk '{print $2}')
    MAX_DELAY=$(grep -m1 "MAX_DELAY:" "$ICORE_YAML" | awk '{print $2}')

    CONTEXT="Pre-synth check: NUM_TAPS=$NUM_TAPS, MAX_DELAY=$MAX_DELAY."

    if [ "$NUM_TAPS" -gt 16 ] 2>/dev/null; then
        CONTEXT="$CONTEXT WARNING: NUM_TAPS=$NUM_TAPS may fail timing on X410 (known issue with NUM_TAPS>16). Consider reducing to 16."
        echo "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"ask\",\"permissionDecisionReason\":\"$CONTEXT\"}}"
    else
        echo "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"$CONTEXT Build will take ~2-3 hours.\"}}"
    fi
else
    exit 0
fi
