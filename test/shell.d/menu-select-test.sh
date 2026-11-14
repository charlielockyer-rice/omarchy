#!/bin/bash

set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/base-test.sh"

require_quickshell "menu select test"

# The shipped Docker entry, and apps the fixture names by desktop id, window
# class and the TUI launcher's app id.
qml_fixture_scratch
applications="$QML_FIXTURE_TMP/home/.local/share/applications"
mkdir -p "$applications"
cp "$ROOT/default/applications/Docker.desktop" "$applications/"
printf '[Desktop Entry]\nType=Application\nName=Fixture Direct\nExec=fixture-direct\nIcon=fixture-direct-icon\nStartupWMClass=FixtureClass\n' >"$applications/fixture-direct.desktop"
printf '[Desktop Entry]\nType=Application\nName=Fixture Top\nExec=fixturetop\nTerminal=true\nIcon=fixture-top\n' >"$applications/fixture-top.desktop"

run_qml_fixture menu-select "menu select draws app icons and confirms a pick in place" Ui Commons
