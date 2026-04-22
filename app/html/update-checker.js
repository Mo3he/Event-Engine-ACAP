'use strict';

/* ===================================================
 * Update Checker — polls GitHub releases for newer versions
 * =================================================== */
const UPDATE_CHECK_REPO = 'Mo3he/Event-Engine-ACAP';
const UPDATE_CHECK_URL  = `https://api.github.com/repos/${UPDATE_CHECK_REPO}/releases/latest`;
const UPDATE_CHECK_KEY  = 'ee_update_dismissed';

/**
 * Compare two semver-like version strings (e.g. "1.9.3" vs "1.10.0").
 * Returns  1 if a > b, -1 if a < b, 0 if equal.
 */
function compareVersions(a, b) {
  const pa = a.replace(/^v/i, '').split('.').map(Number);
  const pb = b.replace(/^v/i, '').split('.').map(Number);
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const na = pa[i] || 0;
    const nb = pb[i] || 0;
    if (na > nb) return 1;
    if (na < nb) return -1;
  }
  return 0;
}

/**
 * Check GitHub for a newer release.  Shows a banner when an update is available.
 * @param {string} currentVersion — the running engine version (e.g. "1.9.3")
 */
async function checkForUpdate(currentVersion, force) {
  if (!currentVersion) return;

  /* Don't re-show if the user dismissed this exact version (unless forced) */
  const dismissed = localStorage.getItem(UPDATE_CHECK_KEY);

  try {
    const resp = await fetch(UPDATE_CHECK_URL, {
      headers: { 'Accept': 'application/vnd.github.v3+json' }
    });
    if (!resp.ok) {
      if (force) toast('Could not reach GitHub (HTTP ' + resp.status + ')', 'error');
      return;
    }
    const data = await resp.json();
    const latest = (data.tag_name || '').replace(/^v/i, '');
    if (!latest) return;

    if (compareVersions(latest, currentVersion) > 0) {
      if (!force && dismissed === latest) return;
      localStorage.removeItem(UPDATE_CHECK_KEY);
      showUpdateBanner(currentVersion, latest, data.html_url);
    } else if (force) {
      toast('You are running the latest version (v' + currentVersion + ')', 'success');
    }
  } catch (_) {
    if (force) toast('Could not reach GitHub — check network connection', 'error');
  }
}

function showUpdateBanner(current, latest, releaseUrl) {
  if (document.getElementById('update-banner')) return;

  const banner = document.createElement('div');
  banner.id = 'update-banner';
  banner.innerHTML =
    `<span>` +
    `<strong>Update available:</strong> Event Engine <strong>v${escHtml(latest)}</strong> is out ` +
    `(you are running v${escHtml(current)}).` +
    `</span> ` +
    `<span class="update-banner-actions">` +
    `<a href="${escHtml(releaseUrl)}" target="_blank" rel="noopener noreferrer" class="btn btn-primary btn-sm">View Release</a>` +
    `<button class="btn btn-ghost btn-sm" id="update-dismiss-btn">Dismiss</button>` +
    `</span>`;

  document.body.appendChild(banner);
  document.getElementById('update-dismiss-btn').addEventListener('click', () => dismissUpdate(latest));
}

function dismissUpdate(version) {
  localStorage.setItem(UPDATE_CHECK_KEY, version);
  const banner = document.getElementById('update-banner');
  if (banner) banner.remove();
}

function manualCheckForUpdate() {
  API.getStatus().then(s => {
    if (s && s.engine_version) checkForUpdate(s.engine_version, true);
  }).catch(() => toast('Failed to get engine status', 'error'));
}
