#!/bin/bash
# send_k_actor_files.sh - Send k_actor texture and geometry files

# Configuration
ENDPOINT="tcp://localhost:13456"
SCRIPT_DIR="/p/project1/ccstvs/george2/jusync/tools/Send data"
PNG_FILE="k_actor__albedoTex_0.000000.png"
USDA_FILE="k_actor__triangles_0_Geom_0.000000.usda"

echo "🚀 Starting k_actor file transfer to ${ENDPOINT}..."
echo "📁 Working directory: ${SCRIPT_DIR}"

# Navigate to the script directory
cd "${SCRIPT_DIR}"

# Verify files exist
if [[ ! -f "${PNG_FILE}" ]]; then
    echo "❌ PNG file not found: ${PNG_FILE}"
    exit 1
fi

if [[ ! -f "${USDA_FILE}" ]]; then
    echo "❌ USDA file not found: ${USDA_FILE}"
    exit 1
fi

echo "✅ Both files found"

# Send PNG file first (texture needs to be available before geometry)
echo ""
echo "📤 Sending texture file: ${PNG_FILE}"
python3 file_sender.py --endpoint "${ENDPOINT}" send-file "${PNG_FILE}"

if [[ $? -ne 0 ]]; then
    echo "❌ Failed to send PNG file"
    exit 1
fi

# Brief pause between transfers
sleep 1

# Send USDA file second (geometry file)
echo ""
echo "📤 Sending geometry file: ${USDA_FILE}"
python3 file_sender.py --endpoint "${ENDPOINT}" send-file "${USDA_FILE}"

if [[ $? -ne 0 ]]; then
    echo "❌ Failed to send USDA file"
    exit 1
fi

echo ""
echo "✅ Successfully sent both k_actor files to ${ENDPOINT}!"
echo "📄 Files sent:"
echo "   1. ${PNG_FILE} (texture)"
echo "   2. ${USDA_FILE} (geometry)"
