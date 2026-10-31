.pragma library

function isLockScreen(name) {
    return name === "kscreenlocker_greet";
}

function isDesktop(name) {
    return name === "plasmashell";
}

function isLoginGreeter(name) {
    // plasma-login-manager renders the login screen wallpaper in its own host
    // process; it is a real on-screen consumer just like plasmashell.
    return name === "plasma-login-wallpaper";
}

function canConnect(name, args) {
    // Greeter previews must not claim the real lock screen's persistent identity.
    return isDesktop(name) || isLoginGreeter(name) || (isLockScreen(name) && args.indexOf("--testing") === -1);
}
