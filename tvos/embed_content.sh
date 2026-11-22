#!/bin/sh
# Copies the imported game contents (tools/import_wad.py, or tools/tvos/port prepare) into
# the app as content/, where the app finds them (platform/tvos/main_tvos.m): tvOS keeps no
# files of its own. Runs as a build step of the app, before Xcode signs it; only changed
# files are copied again. The app is for the player's own devices only.
#   embed_content.sh <content folder>
set -e
dest="${TARGET_BUILD_DIR:?run this from Xcode}/${CONTENTS_FOLDER_PATH:?}/content"
mkdir -p "$dest"
rsync -a --delete --exclude .DS_Store "$1/" "$dest/"
