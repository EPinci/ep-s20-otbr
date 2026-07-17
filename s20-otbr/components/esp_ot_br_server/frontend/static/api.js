'use strict';

/**
 * Escape HTML special characters to prevent XSS when inserting
 * device-supplied strings into innerHTML.
 */
function escapeHtml(str) {
  if (typeof str !== 'string')
    str = String(str);
  return str.replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function apiGet(url) {
  return fetch(url).then(function (r) { return r.json(); }).then(function (data) {
    try {
      sessionStorage.setItem('cache:' + url, JSON.stringify(data));
    } catch (e) {
    }
    return data;
  });
}

function apiPost(url, data) {
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  })
    .then(function (r) { return r.json(); });
}

function apiPut(url, data) {
  return fetch(url, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  }).then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.text().then(function (t) {
      if (!t) return null;
      try { return JSON.parse(t); } catch (e) { return t; }
    });
  });
}

/**
 * Get cached response for a GET endpoint, or null if not available.
 * Use this to render immediately on page load before the live fetch completes.
 */
function getCached(url) {
  try {
    var raw = sessionStorage.getItem('cache:' + url);
    return raw ? JSON.parse(raw) : null;
  } catch (e) {
    return null;
  }
}

function showToast(msg, type) {
  var t = document.getElementById('_toast');
  if (!t) {
    t = document.createElement('div');
    t.id = '_toast';
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.className = 'toast toast-' + (type || 'info') + ' show';
  clearTimeout(t._tid);
  t._tid = setTimeout(function () { t.classList.remove('show'); }, 3500);
}

function showLoading(show) {
  var o = document.getElementById('_loading');
  if (!o) {
    o = document.createElement('div');
    o.id = '_loading';
    o.className = 'loading';
    o.innerHTML =
      '<div class="spinner" style="width:40px;height:40px"></div><div>Loading...</div>';
    document.body.appendChild(o);
  }
  if (show)
    o.classList.add('active');
  else
    o.classList.remove('active');
}

/**
 * Check Thread network state. Calls callback(role) where role is a string
 * like "disabled", "detached", "child", "router", "leader".
 * Returns true if the device is attached (child/router/leader).
 */
function checkThreadState(callback) {
  apiGet('/node/state')
    .then(function (role) {
      var r = (typeof role === 'string') ? role : '';
      var connected = (r === 'child' || r === 'router' || r === 'leader');
      if (callback)
        callback(r, connected);
    })
    .catch(function () {
      if (callback)
        callback('', false);
    });
}

/**
 * Fetch the firmware update status from the device (GET /ota/check).
 * Calls cb(result) with the parsed result object, or cb(null) on any failure.
 * The device answers instantly from cache; while it queries GitHub in the
 * background it replies with status "checking", so re-poll a few times until a
 * final result lands. The check is best-effort: connectivity failures must
 * never break a page.
 */
function fetchUpdateStatus(cb, _attempt) {
  _attempt = _attempt || 0;
  apiGet('/ota/check')
    .then(function (data) {
      var r = data && data.result ? data.result : null;
      if (r && r.status === 'checking' && _attempt < 5) {
        setTimeout(function () { fetchUpdateStatus(cb, _attempt + 1); }, 3000);
        return;
      }
      cb(r);
    })
    .catch(function () {
      cb(null);
    });
}

/**
 * Reveal the firmware update banner and populate its variable fields. The banner
 * markup lives in each page's HTML; this only fills in the version numbers and
 * the release link. The banner is shown only when a newer release is available;
 * when the device is up to date (or the check fails) nothing is displayed.
 */
function renderUpdateBanner(cardId) {
  fetchUpdateStatus(function (r) {
    if (!r || r.status !== 'update_available' || !r.update_available) return;
    var card = document.getElementById(cardId);
    if (!card) return;
    var latest = card.querySelector('.update-latest');
    var current = card.querySelector('.update-current');
    var notes = card.querySelector('.update-notes');
    if (latest) latest.textContent = r.latest;
    if (current) current.textContent = r.current;
    if (notes) {
      if (r.release_url) {
        notes.href = r.release_url;
        notes.classList.remove('hidden');
      } else {
        notes.classList.add('hidden');
      }
    }
    card.classList.remove('hidden');
  });
}

/**
 * Disable all interactive elements inside a management page and show
 * a banner when the Thread network is not connected.
 * Call from management sub-pages (except network.html which is always active).
 */
function disableManagementPage() {
  /* Insert banner at top of .container */
  var container = document.querySelector('.container');
  if (container) {
    var banner = document.createElement('div');
    banner.className = 'disabled-banner';
    banner.innerHTML =
      'Thread network is not connected. <a href="/network.html">Go to Network</a> to join or form a network.';
    container.insertBefore(banner, container.firstChild);
  }
  /* Disable all buttons and inputs */
  var els = document.querySelectorAll('button, input, select, textarea');
  for (var i = 0; i < els.length; i++) {
    els[i].disabled = true;
  }
  /* Add visual overlay to cards */
  var cards = document.querySelectorAll('.card');
  for (var j = 0; j < cards.length; j++) {
    cards[j].classList.add('card-disabled');
  }
}

/* Build navigation bar and highlight current page. */
(function () {
  var pages = [
    { href: '/index.html', label: 'Dashboard' },
    {
      label: 'Thread', children: [
        { href: '/mesh.html', label: 'Mesh' },
        { href: '/commission.html', label: 'Commissioner' },
        { href: '/addresses.html', label: 'Addresses' },
        { href: '/topology.html', label: 'Topology' },
        { href: '/tools.html', label: 'Tools' }
      ]
    },
    {
      label: 'System', children: [
        { href: '/network.html', label: 'Network' },
        { href: '/logs.html', label: 'Logs' },
        { href: '/ota.html', label: 'OTA Update' },
        { href: '/advanced.html', label: 'Advanced' }
      ]
    },
    { href: '/about.html', label: 'About' }
  ];
  var nav = document.getElementById('nav');
  if (!nav)
    return;
  var p = location.pathname;

  var html =
    '<a href="/" class="nav-brand"><img src="/favicon.ico" width="22" height="22" alt="Espressif"> Espressif Thread Border Router</a><ul class="nav-links">';
  for (var i = 0; i < pages.length; i++) {
    var pg = pages[i];
    if (pg.children) {
      /* Check if any child is active */
      var childActive = false;
      for (var c = 0; c < pg.children.length; c++) {
        if (p === pg.children[c].href) {
          childActive = true;
          break;
        }
      }
      html += '<li class="nav-dropdown">';
      html += '<a href="#" class="nav-dropdown-toggle' +
        (childActive ? ' active' : '') + '">' + pg.label + '</a>';
      html += '<ul class="nav-dropdown-menu">';
      for (var j = 0; j < pg.children.length; j++) {
        var ch = pg.children[j];
        var active = (p === ch.href) ? ' class="active"' : '';
        html += '<li><a href="' + ch.href + '"' + active + '>' + ch.label +
          '</a></li>';
      }
      html += '</ul></li>';
    } else {
      var active = (p === pg.href || (p === '/' && pg.href === '/index.html'))
        ? ' class="active"'
        : '';
      html +=
        '<li><a href="' + pg.href + '"' + active + '>' + pg.label + '</a></li>';
    }
  }
  html += '</ul>';
  nav.innerHTML = html;
})();
