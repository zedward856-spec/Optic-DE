// Optic: Firefox tuned for proot on a phone (fast start, no phoning home) and the Optic look.
user_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);
// Tabs in the title bar (Firefox's own), with its buttons restyled as Optic's (userChrome.css).
user_pref("browser.tabs.inTitlebar", 1);
user_pref("browser.theme.content-theme", 0);
user_pref("browser.theme.toolbar-theme", 0);
user_pref("ui.systemUsesDarkTheme", 1);
user_pref("layout.css.prefers-color-scheme.content-override", 0);
// Start faster: fewer processes, nothing to check or show first.
user_pref("dom.ipc.processCount", 2);
user_pref("fission.autostart", false);
user_pref("browser.startup.preXulSkeletonUI", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("app.update.auto", false);
user_pref("app.normandy.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("toolkit.telemetry.unified", false);
user_pref("extensions.pocket.enabled", false);
user_pref("browser.newtabpage.activity-stream.feeds.topsites", false);
user_pref("browser.newtabpage.activity-stream.feeds.section.topstories", false);
user_pref("browser.newtabpage.activity-stream.showSponsored", false);
user_pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
user_pref("browser.translations.enable", false);
user_pref("network.prefetch-next", false);
// No GPU in proot: software rendering from the start.
user_pref("gfx.webrender.software", true);
user_pref("media.hardware-video-decoding.enabled", false);
// The preloaded Firefox ends with the desktop, not by Quit: no "Open previous tabs?" bar each time.
user_pref("browser.startup.couldRestoreSession.count", 2);
