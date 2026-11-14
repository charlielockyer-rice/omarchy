echo "Match T3 Code's windows to its launcher, so the dock shows one icon for it"

# Older releases named the launcher's window class t3code, but the app's
# windows are com.t3tools.T3Code. The app updates itself and never rewrites the
# launcher the installer copied, so fix the copy here.
desktop="$HOME/.local/share/applications/t3code.desktop"
if [[ -f $desktop ]] && grep -qx 'StartupWMClass=t3code' "$desktop"; then
  sed -i 's/^StartupWMClass=t3code$/StartupWMClass=com.t3tools.T3Code/' "$desktop"
fi
