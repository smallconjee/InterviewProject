'use strict';
// ============================================================================
// app.js — PACS Web 控制台（零依赖原生 JS，hash 路由单页应用）
//
// 后端：同源 wfrest HTTP 服务（默认 http://localhost:8080），API 前缀 /api/v1；
//       RIS 报告检索经 PACS 的 TLV 桥接接口访问（浏览器不直连 TCP 9090）。
// 会话：登录换 JWT 存 localStorage；role 不在登录响应里，从 token 的
//       payload 段解码（base64url），过期即清除。
// 兼容：既有接口 Content-Type 是 text/plain，故统一 res.text() + JSON.parse。
// 页面：检查列表 / 检查详情 / 报告检索 / 影像导入 / 实例状态 / 备份任务(admin)
// ============================================================================

// ---------------- 基础工具 ----------------

function $(sel, el) { return (el || document).querySelector(sel); }

// HTML 转义：所有后端数据进模板前必须经过这里（患者名等含中文/特殊字符）
function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

function fmtSize(n) {
  if (n == null) return '-';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}

// base64url 解码 JWT payload（第二段），按 UTF-8 还原中文
function jwtDecode(token) {
  try {
    var s = token.split('.')[1].replace(/-/g, '+').replace(/_/g, '/');
    while (s.length % 4) s += '=';
    var bytes = Uint8Array.from(atob(s), function (c) { return c.charCodeAt(0); });
    return JSON.parse(new TextDecoder().decode(bytes));
  } catch (e) {
    return null;
  }
}

var toastTimer = null;
function toast(msg, kind) {
  var t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (kind === 'err' ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function () { t.classList.add('hidden'); }, 2600);
}

// 状态徽章色系：与后端两个状态机（归档/备份 + 消息表）一一对应
var BADGE_KIND = {
  ARCHIVED: 'ok', CONFIRMED: 'ok', BACKED_UP: 'ok',
  RECEIVED: 'warn', PARSED: 'warn', PENDING: 'warn',
  FAILED: 'err', CONFLICT: 'err', DEAD: 'err',
  PUBLISHED: 'info', DUPLICATE: 'info'
};

function badge(text, extra) {
  var kind = BADGE_KIND[text] || 'mut';
  return '<span class="badge ' + kind + (extra ? ' ' + extra : '') + '">' + esc(text) + '</span>';
}

// ---------------- 会话 ----------------

var TOKEN_KEY = 'pacs_token';
var session = null; // {token, username, role, exp}

function loadSession() {
  var t = null;
  try { t = localStorage.getItem(TOKEN_KEY); } catch (e) { /* 隐私模式等 */ }
  if (!t) return null;
  var p = jwtDecode(t);
  if (!p || (p.exp && p.exp * 1000 < Date.now())) {
    try { localStorage.removeItem(TOKEN_KEY); } catch (e) {}
    return null;
  }
  return { token: t, username: p.sub || '', role: p.role || '', exp: p.exp || 0 };
}

function saveSession(token) {
  var p = jwtDecode(token) || {};
  session = { token: token, username: p.sub || '', role: p.role || '', exp: p.exp || 0 };
  try { localStorage.setItem(TOKEN_KEY, token); } catch (e) {}
}

function clearSession() {
  session = null;
  try { localStorage.removeItem(TOKEN_KEY); } catch (e) {}
}

function isAdmin() { return session && session.role === 'admin'; }

// ---------------- API 封装 ----------------

// 统一入口：带 Bearer 头；401 一律清会话弹登录；返回 {status, data}
async function api(path, opts) {
  opts = opts || {};
  var headers = Object.assign({}, opts.headers || {});
  if (session && session.token) headers['Authorization'] = 'Bearer ' + session.token;
  var res = await fetch(path, Object.assign({}, opts, { headers: headers }));
  var text = await res.text();
  var data = null;
  try { data = JSON.parse(text); } catch (e) { data = { code: -1, message: text }; }
  if (res.status === 401) {
    clearSession();
    showLogin('登录已过期，请重新登录');
    throw new Error('unauthorized');
  }
  return { status: res.status, data: data };
}

// ---------------- 路由 ----------------

// '#/studies/xxx?uid=1' → { parts: ['studies','xxx'], query: {uid:'1'} }
function parseHash() {
  var h = (location.hash || '#/studies').slice(1);
  var pair = h.split('?');
  var parts = pair[0].split('/').filter(Boolean);
  var query = {};
  if (pair[1]) {
    pair[1].split('&').forEach(function (kv) {
      var p = kv.split('=');
      if (p[0]) query[decodeURIComponent(p[0])] = decodeURIComponent(p[1] || '');
    });
  }
  return { parts: parts, query: query };
}

var NAV = [
  { page: 'studies', hash: '#/studies', label: '📋 检查列表' },
  { page: 'reports', hash: '#/reports', label: '🔍 报告检索' },
  { page: 'import', hash: '#/import', label: '⬆️ 影像导入' },
  { page: 'instances', hash: '#/instances', label: '🩻 实例状态' },
  { page: 'backup', hash: '#/backup', label: '🗄️ 备份任务', admin: true }
];

function renderTopbar() {
  var cur = parseHash().parts[0] || 'studies';
  var nav = NAV.filter(function (n) { return !n.admin || isAdmin(); });
  $('#nav').innerHTML = nav.map(function (n) {
    return '<a href="' + n.hash + '" class="' + (n.page === cur ? 'active' : '') + '">' +
      n.label + '</a>';
  }).join('');
  $('#userbox').innerHTML = session
    ? '<span class="u-name">' + esc(session.username) + '</span>' +
      '<span class="badge b-navy">' + esc(session.role === 'admin' ? '管理员' : '放射医师') + '</span>' +
      '<button class="btn ghost sm" id="btn-logout">退出</button>'
    : '';
  var btn = $('#btn-logout');
  if (btn) btn.onclick = function () {
    clearSession();
    showLogin('');
    toast('已退出登录');
  };
}

function render() {
  renderTopbar();
  var root = $('#view');
  if (!session) { root.innerHTML = ''; return; }
  var r = parseHash();
  var page = r.parts[0] || 'studies';
  switch (page) {
    case 'studies':
      if (r.parts[1]) viewStudyDetail(root, decodeURIComponent(r.parts[1]));
      else viewStudies(root);
      break;
    case 'reports': viewReports(root); break;
    case 'import': viewImport(root); break;
    case 'instances': viewInstances(root, r.query.uid || ''); break;
    case 'backup': viewBackup(root); break;
    default: viewStudies(root);
  }
}

window.addEventListener('hashchange', render);

// ---------------- 登录 ----------------

function showLogin(errMsg) {
  $('#view').innerHTML = '';
  renderTopbar();
  $('#login-overlay').classList.remove('hidden');
  var err = $('#login-error');
  if (errMsg) { err.textContent = errMsg; err.classList.remove('hidden'); }
  else err.classList.add('hidden');
}

async function doLogin(e) {
  e.preventDefault();
  var form = e.target;
  var btn = form.querySelector('button[type=submit]');
  btn.disabled = true;
  btn.textContent = '登录中…';
  try {
    var res = await fetch('/api/v1/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        username: form.username.value.trim(),
        password: form.password.value
      })
    });
    var text = await res.text();
    var data = {};
    try { data = JSON.parse(text); } catch (err) {}
    if (res.status === 200 && data.token) {
      saveSession(data.token);
      $('#login-overlay').classList.add('hidden');
      render();
      toast('欢迎，' + session.username);
    } else {
      var err2 = $('#login-error');
      err2.textContent = data.message || ('登录失败（HTTP ' + res.status + '）');
      err2.classList.remove('hidden');
    }
  } catch (err) {
    var err3 = $('#login-error');
    err3.textContent = '无法连接服务：' + err.message;
    err3.classList.remove('hidden');
  } finally {
    btn.disabled = false;
    btn.textContent = '登 录';
  }
}

// ---------------- 视图：检查列表 ----------------

async function viewStudies(root) {
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>📋 检查列表</h2>' +
    '    <span class="muted">按检查日期倒序 · 最多 200 条 · 点击行查看序列</span></div>' +
    '  <form class="filter-bar" id="study-filter">' +
    '    <input name="patient_id" placeholder="患者ID，如 P001" style="width:180px">' +
    '    <input name="issuer" placeholder="签发机构，如 HOSPITAL_A" style="width:200px">' +
    '    <button class="btn" type="submit">筛选</button>' +
    '    <button class="btn ghost" type="button" id="study-reset">重置</button>' +
    '  </form>' +
    '  <div id="study-tbl"></div>' +
    '</div>';

  var load = async function (pid, issuer) {
    var box = $('#study-tbl');
    box.innerHTML = '<div class="loading">加载中…</div>';
    var qs = [];
    if (pid) qs.push('patient_id=' + encodeURIComponent(pid));
    if (issuer) qs.push('issuer=' + encodeURIComponent(issuer));
    try {
      var r = await api('/api/v1/studies' + (qs.length ? '?' + qs.join('&') : ''));
      if (r.status !== 200 || r.data.code !== 0) {
        box.innerHTML = '<div class="err-card">查询失败：' + esc(r.data.message || r.status) + '</div>';
        return;
      }
      var rows = r.data.studies || [];
      if (!rows.length) {
        box.innerHTML = '<div class="empty">没有检查数据。<br>' +
          '<span class="muted">可先在「影像导入」上传 DICOM，或运行 scripts/demo_pacs.sh 造演示数据</span></div>';
        return;
      }
      var tr = rows.map(function (s) {
        return '<tr class="rowlink" data-uid="' + esc(s.study_instance_uid) + '">' +
          '<td>' + esc(s.study_date) + '</td>' +
          '<td><b>' + esc(s.patient_name) + '</b></td>' +
          '<td class="mono">' + esc(s.patient_id) + '</td>' +
          '<td>' + esc(s.issuer) + '</td>' +
          '<td>' + esc(s.study_description) + '</td>' +
          '<td class="mono">' + esc(s.accession_number) + '</td>' +
          '<td>' + s.instance_count + ' 张</td></tr>';
      }).join('');
      box.innerHTML =
        '<table class="tbl"><thead><tr>' +
        '<th>检查日期</th><th>患者</th><th>患者ID</th><th>机构</th>' +
        '<th>检查描述</th><th>检查号</th><th>影像数</th></tr></thead>' +
        '<tbody>' + tr + '</tbody></table>';
      box.querySelectorAll('tr.rowlink').forEach(function (tr2) {
        tr2.onclick = function () {
          location.hash = '#/studies/' + encodeURIComponent(tr2.dataset.uid);
        };
      });
    } catch (err) {
      if (err.message !== 'unauthorized')
        box.innerHTML = '<div class="err-card">请求异常：' + esc(err.message) + '</div>';
    }
  };

  $('#study-filter').onsubmit = function (e) {
    e.preventDefault();
    load(this.patient_id.value.trim(), this.issuer.value.trim());
  };
  $('#study-reset').onclick = function () {
    $('#study-filter').reset();
    load('', '');
  };
  load('', '');
}

// ---------------- 视图：检查详情（序列列表） ----------------

async function viewStudyDetail(root, uid) {
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>🩻 检查详情</h2>' +
    '    <a href="#/studies" class="muted">← 返回列表</a></div>' +
    '  <div class="muted" style="margin-bottom:14px">StudyInstanceUID：<span class="mono">' +
    esc(uid) + '</span></div>' +
    '  <div id="series-tbl"><div class="loading">加载中…</div></div>' +
    '</div>';
  try {
    var r = await api('/api/v1/studies/' + encodeURIComponent(uid));
    var box = $('#series-tbl');
    if (r.status !== 200 || r.data.code !== 0) {
      box.innerHTML = '<div class="err-card">查询失败：' + esc(r.data.message || r.status) + '</div>';
      return;
    }
    var rows = r.data.series || [];
    if (!rows.length) {
      box.innerHTML = '<div class="empty">该检查下没有序列数据</div>';
      return;
    }
    box.innerHTML =
      '<table class="tbl"><thead><tr>' +
      '<th>序列号</th><th>检查类型</th><th>序列 UID</th><th>影像数</th></tr></thead><tbody>' +
      rows.map(function (s) {
        return '<tr><td>' + s.series_number + '</td>' +
          '<td><span class="badge b-info">' + esc(s.modality) + '</span></td>' +
          '<td class="mono">' + esc(s.series_instance_uid) + '</td>' +
          '<td>' + s.instance_count + ' 张</td></tr>';
      }).join('') + '</tbody></table>';
  } catch (err) {
    if (err.message !== 'unauthorized')
      $('#series-tbl').innerHTML = '<div class="err-card">请求异常：' + esc(err.message) + '</div>';
  }
}

// ---------------- 视图：影像导入 ----------------

async function viewImport(root) {
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>⬆️ 影像导入</h2>' +
    '    <span class="muted">multipart 上传 → 解析 → SHA-256 去重 → 入库 → 发布备份消息</span></div>' +
    '  <form id="import-form">' +
    '    <label>DICOM 文件（.dcm）' +
    '      <input type="file" name="file" accept=".dcm,application/dcm" required></label>' +
    '    <button class="btn" type="submit">上传归档</button>' +
    '  </form>' +
    '  <div class="demo-tip">演示数据：testdata/dicom/ 下的 *.dcm（scripts/gen_dicom.py 生成）。' +
    '重复上传同一文件演示幂等（DUPLICATE），改动字节后重传演示冲突拒绝（CONFLICT）。</div>' +
    '  <div id="import-result"></div>' +
    '</div>';

  $('#import-form').onsubmit = async function (e) {
    e.preventDefault();
    var fileInput = this.file;
    var btn = this.querySelector('button[type=submit]');
    if (!fileInput.files.length) return;
    btn.disabled = true;
    btn.textContent = '上传中…';
    $('#import-result').innerHTML = '';
    try {
      var fd = new FormData();
      fd.append('file', fileInput.files[0]);
      var res = await fetch('/api/v1/images/import', {
        method: 'POST',
        headers: { Authorization: 'Bearer ' + session.token },
        body: fd
      });
      var text = await res.text();
      var data = {};
      try { data = JSON.parse(text); } catch (err) {}
      $('#import-result').innerHTML =
        '<div class="card" style="margin-top:16px;box-shadow:none;border:1px solid var(--border)">' +
        '  <div class="hit-head" style="margin-bottom:10px">' +
        badge(data.status || 'FAILED', 'lg') +
        '    <span class="muted">HTTP ' + res.status + '</span></div>' +
        '  <table class="kv">' +
        '    <tr><th>说明</th><td>' + esc(data.detail || data.message || '-') + '</td></tr>' +
        '    <tr><th>实例 ID</th><td>' + (data.instance_id != null ? data.instance_id : '-') + '</td></tr>' +
        '    <tr><th>备份任务</th><td class="mono wrap">' + esc(data.task_id || '-') + '</td></tr>' +
        '    <tr><th>文件名</th><td class="mono">' + esc(fileInput.files[0].name) + '</td></tr>' +
        '  </table></div>';
      toast(data.status === 'ARCHIVED' ? '归档成功' : '处理完成：' + (data.status || 'FAILED'));
    } catch (err) {
      $('#import-result').innerHTML =
        '<div class="err-card">上传异常：' + esc(err.message) + '</div>';
    } finally {
      btn.disabled = false;
      btn.textContent = '上传归档';
    }
  };
}

// ---------------- 视图：实例状态 ----------------

function viewInstances(root, prefill) {
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>🩻 实例状态查询</h2>' +
    '    <span class="muted">归档状态机 RECEIVED→PARSED→ARCHIVED · 备份 NONE→PENDING→BACKED_UP</span></div>' +
    '  <form class="filter-bar" id="inst-form">' +
    '    <input name="uid" placeholder="SOPInstanceUID" style="width:420px" value="' + esc(prefill) + '">' +
    '    <button class="btn" type="submit">查询</button>' +
    '  </form>' +
    '  <div id="inst-result"></div>' +
    '</div>';

  var lookup = async function (uid) {
    var box = $('#inst-result');
    box.innerHTML = '<div class="loading">查询中…</div>';
    try {
      var r = await api('/api/v1/instances/' + encodeURIComponent(uid));
      if (r.status === 404) {
        box.innerHTML = '<div class="empty">未找到该实例<br>' +
          '<span class="muted">确认 SOPInstanceUID 是否正确（可从导入日志或 testdata 命名中获取）</span></div>';
        return;
      }
      if (r.status !== 200 || r.data.code !== 0) {
        box.innerHTML = '<div class="err-card">查询失败：' + esc(r.data.message || r.status) + '</div>';
        return;
      }
      var d = r.data;
      box.innerHTML =
        '<div class="card" style="box-shadow:none;border:1px solid var(--border)">' +
        '  <div class="hit-head" style="margin-bottom:12px">' +
        '    <span class="muted">归档状态</span>' + badge(d.status, 'lg') +
        '    <span class="muted" style="margin-left:16px">备份状态</span>' + badge(d.backup_status, 'lg') +
        '  </div>' +
        '  <table class="kv">' +
        '    <tr><th>SOPInstanceUID</th><td class="mono wrap">' + esc(d.sop_instance_uid) + '</td></tr>' +
        '    <tr><th>SHA-256</th><td class="mono wrap">' + esc(d.sha256) + '</td></tr>' +
        '    <tr><th>文件大小</th><td>' + fmtSize(d.file_size) + '</td></tr>' +
        '    <tr><th>存储路径</th><td class="mono wrap">' + esc(d.storage_path) + '</td></tr>' +
        '  </table></div>';
    } catch (err) {
      if (err.message !== 'unauthorized')
        box.innerHTML = '<div class="err-card">请求异常：' + esc(err.message) + '</div>';
    }
  };

  $('#inst-form').onsubmit = function (e) {
    e.preventDefault();
    var uid = this.uid.value.trim();
    if (uid) lookup(uid);
  };
  if (prefill) lookup(prefill);
}

// ---------------- 视图：备份任务（仅 admin） ----------------

async function viewBackup(root) {
  if (!isAdmin()) {
    root.innerHTML =
      '<div class="card"><div class="card-head"><h2>🗄️ 备份任务</h2></div>' +
      '<div class="err-card">仅管理员（admin）可查看备份任务流水。' +
      '当前角色：' + esc(session ? session.role : '-') + '</div></div>';
    return;
  }
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>🗄️ 备份任务流水</h2>' +
    '    <div style="display:flex;gap:10px;align-items:center">' +
    '      <span class="muted">本地消息表最近 20 条 · PENDING→PUBLISHED→CONFIRMED / DEAD</span>' +
    '      <button class="btn ghost sm" id="bk-refresh">刷新</button></div></div>' +
    '  <div id="bk-tbl"></div>' +
    '</div>';

  var load = async function () {
    var box = $('#bk-tbl');
    box.innerHTML = '<div class="loading">加载中…</div>';
    try {
      var r = await api('/api/v1/admin/backup-status');
      if (r.status !== 200 || r.data.code !== 0) {
        box.innerHTML = '<div class="err-card">查询失败：' + esc(r.data.message || r.status) + '</div>';
        return;
      }
      var rows = r.data.events || [];
      if (!rows.length) {
        box.innerHTML = '<div class="empty">暂无备份事件<br>' +
          '<span class="muted">导入一条影像后，这里会出现 PUBLISHED 流水（consumer 确认后变 CONFIRMED）</span></div>';
        return;
      }
      box.innerHTML =
        '<table class="tbl"><thead><tr>' +
        '<th>任务 ID（幂等键）</th><th>实例</th><th>状态</th><th>重试次数</th></tr></thead><tbody>' +
        rows.map(function (e2) {
          return '<tr><td class="mono wrap" style="max-width:340px">' + esc(e2.task_id) + '</td>' +
            '<td>' + e2.instance_id + '</td>' +
            '<td>' + badge(e2.status) + '</td>' +
            '<td>' + e2.retry_count + '</td></tr>';
        }).join('') + '</tbody></table>';
    } catch (err) {
      if (err.message !== 'unauthorized')
        box.innerHTML = '<div class="err-card">请求异常：' + esc(err.message) + '</div>';
    }
  };

  $('#bk-refresh').onclick = load;
  load();
}

// ---------------- 视图：报告检索（RIS 桥接） ----------------

// snippet 关键词高亮：先后端数据 esc，再把查询词包上 <mark>
function highlight(text, q) {
  var safe = esc(text);
  if (!q) return safe;
  var eq = esc(q);
  return safe.split(eq).join('<mark>' + eq + '</mark>');
}

function viewReports(root) {
  root.innerHTML =
    '<div class="card">' +
    '  <div class="card-head"><h2>🔍 报告检索（RIS）</h2>' +
    '    <span class="muted">TLV 桥接 → jieba 分词 → 倒排召回 → TF-IDF → 两级缓存</span></div>' +
    '  <form class="filter-bar" id="ris-form">' +
    '    <input name="q" placeholder="输入关键词，如：磨玻璃 / 结节 / 骨折 / 肺气肿" style="width:360px">' +
    '    <button class="btn" type="submit">搜索</button>' +
    '  </form>' +
    '  <div id="ris-suggest"></div>' +
    '  <div id="ris-summary" class="muted" style="margin-bottom:10px"></div>' +
    '  <div id="ris-result"></div>' +
    '</div>';

  var doSuggest = async function (q) {
    var box = $('#ris-suggest');
    try {
      var r = await api('/api/v1/reports/suggest?query=' + encodeURIComponent(q));
      if (r.status !== 200 || !r.data.suggestions) { box.innerHTML = ''; return; }
      var terms = r.data.suggestions
        .map(function (s) { return s.term; })
        .filter(function (t) { return t && t !== q; });
      if (!terms.length) { box.innerHTML = ''; return; }
      // BK-tree 编辑距离 ≤2 的纠错建议：点击即按建议词重搜
      box.innerHTML = '<div class="suggest-bar">你是不是想找：' +
        terms.map(function (t) {
          return '<span class="chip" data-term="' + esc(t) + '">' + esc(t) + '</span>';
        }).join('') + '</div>';
      box.querySelectorAll('.chip').forEach(function (c) {
        c.onclick = function () {
          $('#ris-form').q.value = c.dataset.term;
          doSearch(c.dataset.term);
        };
      });
    } catch (err) { box.innerHTML = ''; }
  };

  var doSearch = async function (q) {
    var box = $('#ris-result');
    var sum = $('#ris-summary');
    var sug = $('#ris-suggest');
    sug.innerHTML = '';
    sum.textContent = '';
    box.innerHTML = '<div class="loading">检索中…</div>';
    try {
      var r = await api('/api/v1/reports/search?query=' + encodeURIComponent(q));
      if (r.status === 502) {
        box.innerHTML = '<div class="err-card">无法连接 RIS 检索服务（TCP 9090）。<br>' +
          '<span class="muted">请确认 04_ris_report_search 已启动；索引需先构建（scripts/demo_ris.sh）</span></div>';
        return;
      }
      if (r.status === 400) {
        box.innerHTML = '<div class="empty">请输入查询词</div>';
        return;
      }
      if (r.status !== 200 || r.data.error) {
        box.innerHTML = '<div class="err-card">检索失败：' +
          esc(r.data.error || r.data.message || ('HTTP ' + r.status)) + '</div>';
        return;
      }
      var hits = r.data.hits || [];
      sum.textContent = '共 ' + (r.data.count || 0) + ' 条' +
        (r.data.version != null ? ' · 索引版本 v' + r.data.version : '');
      if (!hits.length) {
        box.innerHTML = '<div class="empty">没有匹配的报告</div>';
      } else {
        box.innerHTML = hits.map(function (h) {
          return '<div class="hit">' +
            '<div class="hit-head">' +
            '  <span class="rid mono">' + esc(h.report_id) + '</span>' +
            '  <span class="badge b-info">' + esc(h.modality) + '</span>' +
            '  <span class="badge ' + (h.has_image ? 'b-ok' : 'b-mut') + '">' +
            (h.has_image ? '已关联影像' : '无影像') + '</span>' +
            '  <span class="spacer"></span>' +
            '  <span class="badge b-mut">相关度 ' + (h.score * 100).toFixed(1) + '%</span>' +
            '</div>' +
            '<div class="hit-meta">' + esc(h.patient) + ' · ' + esc(h.date) + '</div>' +
            '<div class="snippet">' + highlight(h.snippet, q) + '</div>' +
            '</div>';
        }).join('');
      }
      // 空结果或每次都拉一把纠错建议（演示 BK-tree；有建议且不同于原词才显示）
      doSuggest(q);
    } catch (err) {
      if (err.message !== 'unauthorized')
        box.innerHTML = '<div class="err-card">请求异常：' + esc(err.message) + '</div>';
    }
  };

  $('#ris-form').onsubmit = function (e) {
    e.preventDefault();
    var q = this.q.value.trim();
    if (q) doSearch(q);
  };
}

// ---------------- 启动 ----------------

$('#login-form').addEventListener('submit', doLogin);
session = loadSession();
if (session) render();
else showLogin('');
