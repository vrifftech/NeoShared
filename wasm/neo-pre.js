// NeoTools browser bootstrap. This runs after the shell creates Module and
// before the generated Emscripten runtime starts.
(function() {
  'use strict';
  if (typeof Module === 'undefined') return;

  var persistentRoot = '/home/web_user/.config/neotools';
  var syncing = false;
  var browserFileSequence = 0;
  var packageDirectorySequence = 0;
  var packageDirectorySessions = new Map();
  var browserWritableFiles = new Map();
  var browserWritableDirectories = new Map();
  var browserWritableTimers = new Map();
  var browserWritablePublishes = new Map();
  var browserWritableTombstones = new Map();
  var browserWriteHooksInstalled = false;
  var retainedFileSetSequence = 0;
  var retainedFileSets = new Map();
  var legacyImportSequence = 0;
  var legacyImportSessions = new Map();
  var legacyImportPathSessions = new Map();
  var legacyImportBytes = 0;

  // Browser resource limits are deliberately below engine and classic-ZIP hard
  // limits. A hosting page may lower them before startup through
  // Module.neoToolsBrowserLimits. Active imports and writable destinations are
  // never evicted to satisfy these limits: a new request is rejected instead.
  var browserLimitOverrides = Module.neoToolsBrowserLimits || {};
  function configuredPositiveInteger(name, fallback) {
    var value = Number(browserLimitOverrides[name]);
    return Number.isSafeInteger(value) && value > 0 ? value : fallback;
  }
  var MAX_PREPARED_DOWNLOADS = configuredPositiveInteger('maxPreparedDownloads', 8);
  var MAX_PREPARED_DOWNLOAD_BYTES = configuredPositiveInteger(
    'maxPreparedDownloadBytes', 512 * 1024 * 1024);
  var MAX_PREPARED_DOWNLOAD_AGE_MS = configuredPositiveInteger(
    'maxPreparedDownloadAgeMs', 15 * 60 * 1000);
  var MAX_FALLBACK_ZIP_BYTES = configuredPositiveInteger(
    'maxFallbackZipBytes', 512 * 1024 * 1024);
  var MAX_LEGACY_IMPORT_SESSIONS = configuredPositiveInteger(
    'maxLegacyImportSessions', 16);
  var MAX_LEGACY_IMPORT_FILES = configuredPositiveInteger(
    'maxLegacyImportFiles', 4096);
  var MAX_LEGACY_IMPORT_FILE_BYTES = configuredPositiveInteger(
    'maxLegacyImportFileBytes', 512 * 1024 * 1024);
  var MAX_LEGACY_IMPORT_SESSION_BYTES = configuredPositiveInteger(
    'maxLegacyImportSessionBytes', 512 * 1024 * 1024);
  var MAX_LEGACY_IMPORT_TOTAL_BYTES = configuredPositiveInteger(
    'maxLegacyImportTotalBytes', 1024 * 1024 * 1024);
  // Disabled by default. Hosts may opt into finite raw-import expiry for
  // compatibility test fixtures or kiosk deployments that knowingly prefer
  // reclamation over persistent document paths.
  var MAX_LEGACY_IMPORT_AGE_MS = configuredPositiveInteger(
    'maxLegacyImportAgeMs', 0);
  var MAX_BROWSER_WRITABLE_OUTPUTS = configuredPositiveInteger(
    'maxBrowserWritableOutputs', 256);
  var MAX_BROWSER_WRITABLE_DIRECTORIES = configuredPositiveInteger(
    'maxBrowserWritableDirectories', 64);
  var MAX_BROWSER_WRITEBACK_BYTES = configuredPositiveInteger(
    'maxBrowserWritebackBytes', 512 * 1024 * 1024);
  var MAX_WRITABLE_TOMBSTONE_AGE_MS = configuredPositiveInteger(
    'maxWritableTombstoneAgeMs', 10000);
  var MAX_IMPORT_PATH_DEPTH = configuredPositiveInteger(
    'maxImportPathDepth', 32);

  function nextRetainedFileSetId() {
    // The C++ bridge carries IDs as uint32_t. Wrap explicitly and skip IDs
    // that are still live rather than relying on JavaScript Number growth.
    for (var attempt = 0; attempt < 0xFFFFFFFF; ++attempt) {
      retainedFileSetSequence = (retainedFileSetSequence + 1) >>> 0;
      if (retainedFileSetSequence !== 0 && !retainedFileSets.has(retainedFileSetSequence)) {
        return retainedFileSetSequence;
      }
    }
    throw new Error('No retained browser-file session IDs are available.');
  }

  function safeBrowserFileName(name, fallback) {
    var value = String(name || '').replace(/[\\/\u0000-\u001f\u007f]/g, '_').trim();
    if (!value || value === '.' || value === '..') value = fallback || 'download.bin';
    return value;
  }

  function normalizedBrowserExtension(extension) {
    var value = String(extension || '').trim().toLowerCase();
    while (value.charAt(0) === '*') value = value.substring(1);
    if (value && value.charAt(0) !== '.') value = '.' + value;
    return /^\.[a-z0-9_+-]+$/.test(value) ? value : '';
  }

  function appendDefaultBrowserExtension(name, extension) {
    var safeName = safeBrowserFileName(name, 'download.bin');
    var normalized = normalizedBrowserExtension(extension);
    if (!normalized) return safeName;
    var dot = safeName.lastIndexOf('.');
    if (dot > 0 && dot + 1 < safeName.length) return safeName;
    return safeName + normalized;
  }

  function uniqueBrowserDirectory(kind) {
    browserFileSequence += 1;
    var directory = '/tmp/neotools-' + kind + '/' +
      Date.now().toString(36) + '-' + browserFileSequence.toString(36);
    FS.mkdirTree(directory);
    return directory;
  }

  function cancelScheduledBrowserDownload(path) {
    path = String(path || '');
    var timer = browserWritableTimers.get(path);
    if (timer) window.clearTimeout(timer);
    browserWritableTimers.delete(path);
  }

  function clearBrowserWritableTombstone(path) {
    path = String(path || '');
    var tombstone = browserWritableTombstones.get(path);
    if (tombstone && tombstone.timer) window.clearTimeout(tombstone.timer);
    browserWritableTombstones.delete(path);
  }

  function setBrowserWritableTombstone(path, entry) {
    path = String(path || '');
    clearBrowserWritableTombstone(path);
    if (!path || !entry) return;
    var tombstone = { entry: entry, timer: 0 };
    tombstone.timer = window.setTimeout(function() {
      var current = browserWritableTombstones.get(path);
      if (current === tombstone) browserWritableTombstones.delete(path);
    }, MAX_WRITABLE_TOMBSTONE_AGE_MS);
    browserWritableTombstones.set(path, tombstone);
  }

  function browserWritableEntry(path) {
    path = String(path || '');
    var entry = browserWritableFiles.get(path);
    if (entry) return entry;
    var tombstone = browserWritableTombstones.get(path);
    return tombstone ? tombstone.entry : null;
  }

  function restoreBrowserWritableTombstone(path) {
    path = String(path || '');
    if (browserWritableFiles.has(path)) return browserWritableFiles.get(path);
    var tombstone = browserWritableTombstones.get(path);
    if (!tombstone) return null;
    clearBrowserWritableTombstone(path);
    browserWritableFiles.set(path, tombstone.entry);
    return tombstone.entry;
  }

  function browserOutputBindingCount() {
    var count = 0;
    for (var entry of browserWritableFiles.values()) {
      if (!entry.importSessionId) count += 1;
    }
    return count;
  }

  function updateLegacyImportSessionPath(entry, oldPath, newPath) {
    if (!entry || !entry.importSessionId || oldPath === newPath) return;
    var session = legacyImportSessions.get(Number(entry.importSessionId));
    if (!session) return;
    var index = session.paths.indexOf(oldPath);
    if (index >= 0) session.paths[index] = newPath;
    legacyImportPathSessions.delete(oldPath);
    legacyImportPathSessions.set(newPath, session.id);
  }

  function forgetBrowserWritablePath(path) {
    path = String(path || '');
    cancelScheduledBrowserDownload(path);
    browserWritableFiles.delete(path);
    clearBrowserWritableTombstone(path);
  }

  function normalizeBrowserWritableDirectory(root) {
    root = String(root || '').replace(/\\/g, '/');
    while (root.length > 1 && root.charAt(root.length - 1) === '/') {
      root = root.substring(0, root.length - 1);
    }
    return root || '/';
  }

  function findBrowserWritableDirectory(path) {
    path = String(path || '');
    var best = null;
    for (var entry of browserWritableDirectories.values()) {
      if (path === entry.root || path.indexOf(entry.root + '/') === 0) {
        if (!best || entry.root.length > best.root.length) best = entry;
      }
    }
    return best;
  }

  function registerBrowserWritableDirectory(root, handle) {
    installBrowserWriteHooks();
    root = normalizeBrowserWritableDirectory(root);
    var existing = browserWritableDirectories.get(root);
    if (!existing && browserWritableDirectories.size >= MAX_BROWSER_WRITABLE_DIRECTORIES) {
      throw new Error('Writable browser directories exceed the configured session limit.');
    }
    browserWritableDirectories.set(root, {
      root: root,
      handle: handle || (existing ? existing.handle : null),
      createdAt: existing ? existing.createdAt : Date.now(),
      lastUsedAt: Date.now()
    });
  }

  function releaseBrowserWritableDirectory(root) {
    root = normalizeBrowserWritableDirectory(root);
    browserWritableDirectories.delete(root);
  }

  async function browserDirectoryFileHandle(entry, relative) {
    var directory = entry.handle;
    var components = String(relative || '').split('/').filter(Boolean);
    if (!components.length) throw new Error('Writable browser path has no filename.');
    var filename = components.pop();
    for (var index = 0; index < components.length; ++index) {
      directory = await directory.getDirectoryHandle(components[index], { create: true });
    }
    return directory.getFileHandle(filename, { create: true });
  }

  async function writeBytesToBrowserFileHandle(handle, bytes) {
    var writable = null;
    try {
      writable = await handle.createWritable();
      await writable.write(bytes);
      await writable.close();
    } catch (error) {
      if (writable && typeof writable.abort === 'function') {
        try { await writable.abort(); } catch (_) {}
      }
      throw error;
    }
  }

  async function publishBrowserWritablePath(path, overrideName) {
    path = String(path || '');
    if (!path) throw new Error('Browser output path is empty.');
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    if (typeof FS.stat === 'function') {
      var fileSize = Number(FS.stat(path).size);
      if (!Number.isSafeInteger(fileSize) || fileSize < 0 ||
          fileSize > MAX_BROWSER_WRITEBACK_BYTES) {
        throw new Error('Browser output exceeds the configured write-back byte limit.');
      }
    }
    var bytes = FS.readFile(path, { encoding: 'binary' });
    if (!Number.isSafeInteger(bytes.byteLength) ||
        bytes.byteLength > MAX_BROWSER_WRITEBACK_BYTES) {
      throw new Error('Browser output exceeds the configured write-back byte limit.');
    }
    var fileEntry = browserWritableFiles.get(path) || restoreBrowserWritableTombstone(path);
    var name = safeBrowserFileName(
      overrideName || (fileEntry ? fileEntry.name : path.split('/').pop()), 'download.bin');

    if (fileEntry && fileEntry.handle) {
      try {
        await writeBytesToBrowserFileHandle(fileEntry.handle, bytes);
        fileEntry.lastUsedAt = Date.now();
        return 1; // Saved to the retained host handle.
      } catch (error) {
        console.warn('[NeoTools] Host-file write failed; preparing a replacement download.', error);
      }
    }

    var directoryEntry = findBrowserWritableDirectory(path);
    if (directoryEntry && directoryEntry.handle) {
      var relative = path.substring(directoryEntry.root.length);
      while (relative.charAt(0) === '/') relative = relative.substring(1);
      if (relative) {
        try {
          var handle = await browserDirectoryFileHandle(directoryEntry, relative);
          await writeBytesToBrowserFileHandle(handle, bytes);
          directoryEntry.lastUsedAt = Date.now();
          return 1;
        } catch (error) {
          console.warn('[NeoTools] Host-directory write failed; preparing a download.', error);
        }
      }
    }

    queueBrowserDownload(bytes, name);
    if (fileEntry) fileEntry.lastUsedAt = Date.now();
    if (directoryEntry) directoryEntry.lastUsedAt = Date.now();
    return 2; // Replacement download prepared.
  }

  function hasBrowserWritableDestination(path) {
    return !!browserWritableEntry(path) || !!findBrowserWritableDirectory(path);
  }

  function enqueueBrowserWritablePublish(path, overrideName) {
    path = String(path || '');
    if (!path) return Promise.reject(new Error('Browser output path is empty.'));
    var previous = browserWritablePublishes.get(path);
    var start = previous ? previous.catch(function() {}) : Promise.resolve();
    var current = start.then(function() {
      return publishBrowserWritablePath(path, overrideName || '');
    });
    browserWritablePublishes.set(path, current);
    current.then(function() {
      if (browserWritablePublishes.get(path) === current) browserWritablePublishes.delete(path);
    }, function() {
      if (browserWritablePublishes.get(path) === current) browserWritablePublishes.delete(path);
    });
    return current;
  }

  function scheduleBrowserWritableDownload(path) {
    path = String(path || '');
    if (!hasBrowserWritableDestination(path)) return;
    cancelScheduledBrowserDownload(path);
    browserWritableTimers.set(path, window.setTimeout(function() {
      browserWritableTimers.delete(path);
      enqueueBrowserWritablePublish(path, '').catch(function(error) {
        console.error('[NeoTools] Unable to publish the browser-written file:', error);
      });
    }, 20));
  }

  function installBrowserWriteHooks() {
    if (browserWriteHooksInstalled || typeof FS === 'undefined') return;
    browserWriteHooksInstalled = true;

    var originalClose = FS.close.bind(FS);
    FS.close = function(stream) {
      var path = '';
      var writable = false;
      try {
        path = String(stream.path || (stream.node ? FS.getPath(stream.node) : '') || '');
        writable = ((stream.flags || 0) & 3) !== 0;
      } catch (_) {}
      var result = originalClose(stream);
      if (path && writable) {
        restoreBrowserWritableTombstone(path);
        scheduleBrowserWritableDownload(path);
      }
      return result;
    };

    var originalRename = FS.rename.bind(FS);
    FS.rename = function(oldPath, newPath) {
      oldPath = String(oldPath || '');
      newPath = String(newPath || '');
      var destinationEntry = browserWritableEntry(newPath);
      var sourceEntry = browserWritableEntry(oldPath);
      var result = originalRename(oldPath, newPath);
      cancelScheduledBrowserDownload(oldPath);
      cancelScheduledBrowserDownload(newPath);
      browserWritableFiles.delete(oldPath);
      browserWritableFiles.delete(newPath);
      clearBrowserWritableTombstone(oldPath);
      clearBrowserWritableTombstone(newPath);
      var selectedEntry = destinationEntry || sourceEntry;
      if (selectedEntry) {
        browserWritableFiles.set(newPath, selectedEntry);
        if (!destinationEntry && sourceEntry) {
          updateLegacyImportSessionPath(sourceEntry, oldPath, newPath);
        }
      }
      scheduleBrowserWritableDownload(newPath);
      return result;
    };

    if (typeof FS.unlink === 'function') {
      var originalUnlink = FS.unlink.bind(FS);
      FS.unlink = function(path) {
        path = String(path || '');
        var entry = browserWritableFiles.get(path) || null;
        var result = originalUnlink(path);
        cancelScheduledBrowserDownload(path);
        if (entry) {
          browserWritableFiles.delete(path);
          setBrowserWritableTombstone(path, entry);
        } else {
          clearBrowserWritableTombstone(path);
        }
        return result;
      };
    }
  }

  function registerBrowserWritablePath(path, name, options) {
    path = String(path || '');
    if (!path) return;
    installBrowserWriteHooks();
    options = options || {};
    var existing = browserWritableEntry(path);
    var importSessionId = Number(options.importSessionId || (existing ? existing.importSessionId : 0) || 0);
    if (!existing && !importSessionId && browserOutputBindingCount() >= MAX_BROWSER_WRITABLE_OUTPUTS) {
      throw new Error('Writable browser files exceed the configured session limit.');
    }
    var entry = existing || {
      createdAt: Date.now(),
      importSessionId: importSessionId
    };
    entry.name = safeBrowserFileName(name || entry.name, 'download.bin');
    entry.handle = options.handle || entry.handle || null;
    entry.importSessionId = importSessionId;
    entry.lastUsedAt = Date.now();
    clearBrowserWritableTombstone(path);
    browserWritableFiles.set(path, entry);
  }

  function consumeScheduledBrowserDownload(path) {
    cancelScheduledBrowserDownload(String(path || ''));
  }

  function browserOpenPickerTypes(accept) {
    var extensions = parseRetainedAcceptExtensions(accept);
    if (!extensions.length) return undefined;
    return [{
      description: 'Supported files',
      accept: { 'application/octet-stream': extensions }
    }];
  }

  async function chooseHostFiles(options) {
    options = options || {};
    if (typeof window.showOpenFilePicker === 'function') {
      try {
        var pickerOptions = { multiple: !!options.multiple };
        var types = browserOpenPickerTypes(options.accept);
        if (types) pickerOptions.types = types;
        var handles = await window.showOpenFilePicker(pickerOptions);
        var handleEntries = [];
        for (var handleIndex = 0; handleIndex < handles.length; ++handleIndex) {
          var file = await handles[handleIndex].getFile();
          handleEntries.push({ file: file, handle: handles[handleIndex], relativePath: file.name });
        }
        return handleEntries;
      } catch (error) {
        if (error && error.name === 'AbortError') return [];
        console.warn('[NeoTools] Native browser file picker failed; using file input.', error);
      }
    }

    var files = await new Promise(function(resolve) {
      var input = document.createElement('input');
      input.type = 'file';
      input.multiple = !!options.multiple;
      if (options.accept) input.accept = options.accept;
      input.style.position = 'fixed';
      input.style.left = '-10000px';
      input.style.top = '-10000px';
      input.setAttribute('aria-hidden', 'true');

      var settled = false;
      var focusTimer = 0;
      function onWindowFocus() {
        focusTimer = window.setTimeout(function() {
          if (!settled && (!input.files || input.files.length === 0)) finish([]);
        }, 350);
      }
      function finish(selected) {
        if (settled) return;
        settled = true;
        if (focusTimer) window.clearTimeout(focusTimer);
        window.removeEventListener('focus', onWindowFocus);
        if (input.parentNode) input.parentNode.removeChild(input);
        resolve(selected || []);
      }

      input.addEventListener('change', function() {
        finish(Array.prototype.slice.call(input.files || []));
      }, { once: true });
      input.addEventListener('cancel', function() { finish([]); }, { once: true });
      window.addEventListener('focus', onWindowFocus);
      document.body.appendChild(input);
      try { input.click(); }
      catch (error) {
        console.error('[NeoTools] Unable to open the browser file picker:', error);
        finish([]);
      }
    });

    return files.map(function(file) {
      return { file: file, handle: null, relativePath: file.name };
    });
  }

  function removeLegacyImportFilesAndDirectories(directory, paths, extraDirectories) {
    var directories = Object.create(null);
    for (var index = 0; index < paths.length; ++index) {
      var path = paths[index];
      try { FS.unlink(path); } catch (_) {}
      var parent = path.substring(0, path.lastIndexOf('/'));
      while (parent && parent.length >= directory.length) {
        directories[parent] = true;
        if (parent === directory) break;
        parent = parent.substring(0, parent.lastIndexOf('/'));
      }
    }
    for (var extraIndex = 0; extraIndex < (extraDirectories || []).length; ++extraIndex) {
      directories[extraDirectories[extraIndex]] = true;
    }
    directories[directory] = true;
    Object.keys(directories).sort(function(left, right) {
      return right.length - left.length;
    }).forEach(function(path) {
      try { FS.rmdir(path); } catch (_) {}
    });
    // Remove now-empty synthetic import parents, stopping safely when another
    // session or an unrelated temporary file keeps an ancestor non-empty.
    var ancestor = directory.substring(0, directory.lastIndexOf('/'));
    while (ancestor && ancestor !== '/' && ancestor !== '/tmp') {
      try { FS.rmdir(ancestor); } catch (_) { break; }
      ancestor = ancestor.substring(0, ancestor.lastIndexOf('/'));
    }
  }

  function finalizeLegacyImportSessionRemoval(session) {
    if (!session || !legacyImportSessions.has(session.id)) return;
    if (session.expiryTimer) window.clearTimeout(session.expiryTimer);
    session.expiryTimer = 0;
    for (var index = 0; index < session.paths.length; ++index) {
      forgetBrowserWritablePath(session.paths[index]);
      legacyImportPathSessions.delete(session.paths[index]);
    }
    removeLegacyImportFilesAndDirectories(session.directory, session.paths);
    legacyImportSessions.delete(session.id);
    legacyImportBytes = Math.max(0, legacyImportBytes - session.bytes);
  }

  function removeLegacyImportSession(session) {
    if (!session || !legacyImportSessions.has(session.id) || session.releasing) return;
    var pendingPublishes = [];
    for (var index = 0; index < session.paths.length; ++index) {
      var path = session.paths[index];
      if (browserWritableTimers.has(path)) {
        cancelScheduledBrowserDownload(path);
        pendingPublishes.push(enqueueBrowserWritablePublish(path, ''));
        continue;
      }
      var activePublish = browserWritablePublishes.get(path);
      if (activePublish) pendingPublishes.push(activePublish);
    }
    if (!pendingPublishes.length) {
      finalizeLegacyImportSessionRemoval(session);
      return;
    }

    // Closing a document immediately after Save must not cancel a delayed,
    // queued, or in-flight host write/download. Flush all path chains first,
    // then reclaim MEMFS.
    session.releasing = true;
    Promise.allSettled(pendingPublishes).then(function(results) {
      for (var resultIndex = 0; resultIndex < results.length; ++resultIndex) {
        if (results[resultIndex].status === 'rejected') {
          console.error('[NeoTools] Unable to publish an imported file before release:',
            results[resultIndex].reason);
        }
      }
      session.releasing = false;
      finalizeLegacyImportSessionRemoval(session);
    });
  }

  function ensureLegacyImportCapacity(fileCount, requiredBytes) {
    if (!Number.isSafeInteger(fileCount) || fileCount < 0 || fileCount > MAX_LEGACY_IMPORT_FILES) {
      throw new Error('The selected files exceed the configured legacy import file-count limit.');
    }
    if (!Number.isSafeInteger(requiredBytes) || requiredBytes < 0 ||
        requiredBytes > MAX_LEGACY_IMPORT_SESSION_BYTES) {
      throw new Error('The selected files exceed the configured legacy import byte limit.');
    }
    if (legacyImportSessions.size >= MAX_LEGACY_IMPORT_SESSIONS ||
        legacyImportBytes + requiredBytes > MAX_LEGACY_IMPORT_TOTAL_BYTES) {
      throw new Error(
        'Imported browser files exceed the active MEMFS budget. Close or release an imported document before opening another file.');
    }
  }

  function releaseLegacyImportedPaths(paths) {
    var values = Array.isArray(paths) ? paths : [paths];
    var sessionIds = Object.create(null);
    for (var index = 0; index < values.length; ++index) {
      var path = String(values[index] || '');
      if (!path) continue;
      var sessionId = legacyImportPathSessions.get(path);
      if (sessionId) sessionIds[sessionId] = true;
    }
    for (var id of Object.keys(sessionIds)) {
      removeLegacyImportSession(legacyImportSessions.get(Number(id)));
    }
  }

  function releaseLegacyImportedDirectory(root) {
    root = normalizeBrowserWritableDirectory(root);
    var sessions = [];
    for (var session of legacyImportSessions.values()) {
      if (normalizeBrowserWritableDirectory(session.root) === root ||
          normalizeBrowserWritableDirectory(session.directory) === root) {
        sessions.push(session);
      }
    }
    for (var index = 0; index < sessions.length; ++index) {
      removeLegacyImportSession(sessions[index]);
    }
  }

  function releaseBrowserOutputPath(path) {
    path = String(path || '');
    if (!path) return Promise.resolve();
    var pending = null;
    if (browserWritableTimers.has(path)) {
      cancelScheduledBrowserDownload(path);
      pending = enqueueBrowserWritablePublish(path, '');
    } else {
      pending = browserWritablePublishes.get(path) || null;
    }
    if (!pending) {
      forgetBrowserWritablePath(path);
      return Promise.resolve();
    }
    return Promise.resolve(pending).catch(function(error) {
      console.error('[NeoTools] Unable to publish a browser output before release:', error);
    }).then(function() {
      forgetBrowserWritablePath(path);
    });
  }

  function releaseBrowserOutputDirectory(root) {
    root = normalizeBrowserWritableDirectory(root);
    var pendingPaths = Object.create(null);
    function underRoot(path) {
      return path === root || path.indexOf(root + '/') === 0;
    }
    for (var timerPath of browserWritableTimers.keys()) {
      if (underRoot(timerPath)) pendingPaths[timerPath] = true;
    }
    for (var publishPath of browserWritablePublishes.keys()) {
      if (underRoot(publishPath)) pendingPaths[publishPath] = true;
    }
    for (var filePath of browserWritableFiles.keys()) {
      var fileEntry = browserWritableFiles.get(filePath);
      if (underRoot(filePath) && fileEntry && !fileEntry.importSessionId) {
        pendingPaths[filePath] = true;
      }
    }
    var releases = Object.keys(pendingPaths).map(function(path) {
      return releaseBrowserOutputPath(path);
    });
    function removeDirectoryBindings() {
      for (var directoryRoot of Array.from(browserWritableDirectories.keys())) {
        if (underRoot(directoryRoot)) browserWritableDirectories.delete(directoryRoot);
      }
    }
    if (!releases.length) {
      removeDirectoryBindings();
      return;
    }
    Promise.allSettled(releases).then(removeDirectoryBindings);
  }

  function normalizedImportRelativePath(value, fallback) {
    var source = String(value || fallback || 'import.bin').replace(/\\/g, '/');
    while (source.charAt(0) === '/') source = source.substring(1);
    var components = source.split('/');
    var result = [];
    for (var index = 0; index < components.length; ++index) {
      var component = components[index];
      if (!component || component === '.') continue;
      if (component === '..') throw new Error('Imported browser paths must not contain parent components.');
      result.push(safeBrowserFileName(component, 'import.bin'));
      if (result.length > MAX_IMPORT_PATH_DEPTH) {
        throw new Error('Imported browser paths exceed the configured nesting-depth limit.');
      }
    }
    if (!result.length) result.push(safeBrowserFileName(fallback, 'import.bin'));
    return result.join('/');
  }

  function uniqueImportRelativePath(relativePath, used) {
    var candidate = relativePath;
    var key = candidate.toLowerCase();
    var suffix = 2;
    while (used[key]) {
      var slash = relativePath.lastIndexOf('/');
      var directory = slash >= 0 ? relativePath.substring(0, slash + 1) : '';
      var leaf = slash >= 0 ? relativePath.substring(slash + 1) : relativePath;
      var dot = leaf.lastIndexOf('.');
      var stem = dot > 0 ? leaf.substring(0, dot) : leaf;
      var extension = dot > 0 ? leaf.substring(dot) : '';
      candidate = directory + stem + '-' + suffix + extension;
      key = candidate.toLowerCase();
      suffix += 1;
    }
    used[key] = true;
    return candidate;
  }

  async function importSelectedHostFiles(entries, options) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    options = options || {};
    if (!entries || !entries.length) return { sessionId: 0, root: '', paths: [] };
    var normalizedEntries = [];
    var totalBytes = 0;
    for (var preflightIndex = 0; preflightIndex < entries.length; ++preflightIndex) {
      var source = entries[preflightIndex] || {};
      var file = source.file || source;
      var size = Number(file.size);
      if (!Number.isSafeInteger(size) || size < 0 || size > MAX_LEGACY_IMPORT_FILE_BYTES) {
        throw new Error('A selected browser file exceeds the configured per-file import limit.');
      }
      totalBytes += size;
      if (!Number.isSafeInteger(totalBytes) || totalBytes > MAX_LEGACY_IMPORT_SESSION_BYTES) {
        throw new Error('The selected files exceed the configured legacy import limit.');
      }
      normalizedEntries.push({
        file: file,
        handle: source.handle || null,
        relativePath: source.relativePath || file.name,
        size: size
      });
    }
    ensureLegacyImportCapacity(normalizedEntries.length, totalBytes);

    var directory = uniqueBrowserDirectory('import');
    var rootName = options.rootName ? safeBrowserFileName(options.rootName, 'folder') : '';
    var sessionRoot = rootName ? directory + '/' + rootName : directory;
    if (rootName) FS.mkdirTree(sessionRoot);
    var used = Object.create(null);
    var records = [];
    var createdDirectories = [];
    try {
      for (var index = 0; index < normalizedEntries.length; ++index) {
        var entry = normalizedEntries[index];
        var originalName = safeBrowserFileName(entry.file.name, 'import.bin');
        var relative = options.preserveRelativePaths
          ? normalizedImportRelativePath(entry.relativePath, originalName)
          : originalName;
        relative = uniqueImportRelativePath(relative, used);
        var path = sessionRoot + '/' + relative;
        var slash = path.lastIndexOf('/');
        if (slash > 0) {
          var parent = path.substring(0, slash);
          FS.mkdirTree(parent);
          var cursor = parent;
          while (cursor && cursor.length >= directory.length) {
            createdDirectories.push(cursor);
            if (cursor === directory) break;
            cursor = cursor.substring(0, cursor.lastIndexOf('/'));
          }
        }
        var bytes = new Uint8Array(await entry.file.arrayBuffer());
        FS.writeFile(path, bytes, { canOwn: true });
        records.push({
          path: path,
          name: originalName,
          handle: entry.handle,
          size: entry.size
        });
      }
    } catch (error) {
      removeLegacyImportFilesAndDirectories(
        directory,
        records.map(function(record) { return record.path; }),
        createdDirectories);
      throw error;
    }

    legacyImportSequence = (legacyImportSequence + 1) >>> 0;
    if (legacyImportSequence === 0) legacyImportSequence = 1;
    while (legacyImportSessions.has(legacyImportSequence)) {
      legacyImportSequence = (legacyImportSequence + 1) >>> 0;
      if (legacyImportSequence === 0) legacyImportSequence = 1;
    }
    var session = {
      id: legacyImportSequence,
      directory: directory,
      root: sessionRoot,
      paths: records.map(function(record) { return record.path; }),
      bytes: totalBytes,
      createdAt: Date.now(),
      expiryTimer: 0
    };
    legacyImportSessions.set(session.id, session);
    if (MAX_LEGACY_IMPORT_AGE_MS > 0) {
      session.expiryTimer = window.setTimeout(function() {
        removeLegacyImportSession(session);
      }, MAX_LEGACY_IMPORT_AGE_MS);
    }
    legacyImportBytes += totalBytes;
    try {
      for (var registerIndex = 0; registerIndex < records.length; ++registerIndex) {
        var record = records[registerIndex];
        legacyImportPathSessions.set(record.path, session.id);
        registerBrowserWritablePath(record.path, record.name, {
          importSessionId: session.id,
          handle: record.handle
        });
      }
    } catch (error) {
      removeLegacyImportSession(session);
      throw error;
    }
    return { sessionId: session.id, root: sessionRoot, paths: session.paths.slice() };
  }

  async function importHostFiles(options) {
    var entries = await chooseHostFiles(options);
    var result = await importSelectedHostFiles(entries, options || {});
    return result.paths;
  }

  function createBrowserWritableFile(name, handle) {
    var safeName = safeBrowserFileName(name, 'download.bin');
    var root = uniqueBrowserDirectory('export');
    var path = root + '/' + safeName;
    try {
      registerBrowserWritablePath(path, safeName, { handle: handle || null });
      // Formats may emit sidecars beside their primary output.
      registerBrowserWritableDirectory(root, null);
      return path;
    } catch (error) {
      forgetBrowserWritablePath(path);
      releaseBrowserWritableDirectory(root);
      try { FS.rmdir(root); } catch (_) {}
      throw error;
    }
  }

  function createBrowserWritableDirectory(name, handle) {
    var parent = uniqueBrowserDirectory('output');
    var root = parent + '/' + safeBrowserFileName(name, 'folder');
    FS.mkdirTree(root);
    try {
      registerBrowserWritableDirectory(root, handle || null);
      return root;
    } catch (error) {
      try { FS.rmdir(root); } catch (_) {}
      try { FS.rmdir(parent); } catch (_) {}
      throw error;
    }
  }

  async function collectLegacyDirectoryEntries(handle) {
    var entries = [];
    var totalBytes = 0;

    async function walk(directory, relativeRoot, depth) {
      if (depth > MAX_IMPORT_PATH_DEPTH) {
        throw new Error('The selected directory exceeds the configured nesting-depth limit.');
      }
      for await (var pair of directory.entries()) {
        var name = safeBrowserFileName(pair[0], 'entry');
        var child = pair[1];
        var relative = relativeRoot ? relativeRoot + '/' + name : name;
        if (child.kind === 'directory') {
          await walk(child, relative, depth + 1);
          continue;
        }
        if (child.kind !== 'file') continue;
        if (entries.length >= MAX_LEGACY_IMPORT_FILES) {
          throw new Error('The selected directory exceeds the configured file-count limit.');
        }
        var file = await child.getFile();
        var size = Number(file.size);
        if (!Number.isSafeInteger(size) || size < 0 || size > MAX_LEGACY_IMPORT_FILE_BYTES) {
          throw new Error('A selected directory file exceeds the configured per-file import limit.');
        }
        totalBytes += size;
        if (!Number.isSafeInteger(totalBytes) || totalBytes > MAX_LEGACY_IMPORT_SESSION_BYTES) {
          throw new Error('The selected directory exceeds the configured import byte limit.');
        }
        entries.push({ file: file, handle: child, relativePath: relative });
      }
    }

    await walk(handle, '', 0);
    return entries;
  }

  async function chooseLegacyDirectory(options) {
    options = options || {};
    var allowCreate = !!options.allowCreate;
    if (typeof window.showDirectoryPicker === 'function') {
      var handle = null;
      try {
        handle = await window.showDirectoryPicker({
          mode: allowCreate ? 'readwrite' : 'read'
        });
      } catch (error) {
        if (error && error.name === 'AbortError') return '';
        console.warn('[NeoTools] Native browser directory picker failed; using fallback.', error);
      }
      if (handle) {
        if (allowCreate) {
          // Output selection only needs a destination capability. Do not copy
          // the existing host directory into MEMFS.
          return createBrowserWritableDirectory(handle.name || 'folder', handle);
        }
        var entries = await collectLegacyDirectoryEntries(handle);
        if (!entries.length) {
          return createBrowserWritableDirectory(handle.name || 'folder', null);
        }
        var result = await importSelectedHostFiles(entries, {
          preserveRelativePaths: true,
          rootName: handle.name || 'folder'
        });
        try {
          // New files created under a read directory are exposed as downloads;
          // existing imported files retain their individual handles.
          registerBrowserWritableDirectory(result.root, null);
          return result.root;
        } catch (error) {
          releaseLegacyImportedPaths(result.paths);
          throw error;
        }
      }
    }

    if (allowCreate) return createBrowserWritableDirectory('output', null);

    var files = await chooseHostDirectoryFiles({ accept: options.accept || '' });
    if (!files.length) return '';
    var firstRelative = String(files[0].webkitRelativePath || files[0].name || 'folder');
    var rootName = safeBrowserFileName(firstRelative.split('/')[0], 'folder');
    var entries = files.map(function(file) {
      var source = String(file.webkitRelativePath || file.name || 'import.bin').replace(/\\/g, '/');
      var parts = source.split('/');
      if (parts.length > 1) parts.shift();
      return { file: file, handle: null, relativePath: parts.join('/') || file.name };
    });
    var result = await importSelectedHostFiles(entries, {
      preserveRelativePaths: true,
      rootName: rootName
    });
    try {
      registerBrowserWritableDirectory(result.root, null);
      return result.root;
    } catch (error) {
      releaseLegacyImportedPaths(result.paths);
      throw error;
    }
  }

  function parseRetainedAcceptExtensions(accept) {
    var values = String(accept || '').toLowerCase().split(',');
    var result = [];
    for (var index = 0; index < values.length; ++index) {
      var value = values[index].trim();
      if (!value) continue;
      var dot = value.lastIndexOf('.');
      if (dot >= 0) value = value.substring(dot);
      if (/^\.[a-z0-9_+-]+$/.test(value) && result.indexOf(value) < 0) result.push(value);
    }
    return result;
  }

  function retainedFileMatches(name, extensions) {
    if (!extensions || !extensions.length) return true;
    var lower = String(name || '').toLowerCase();
    for (var index = 0; index < extensions.length; ++index) {
      if (lower.endsWith(extensions[index])) return true;
    }
    return false;
  }

  function normalizeRetainedRelativePath(value) {
    var source = String(value || '').replace(/\\/g, '/');
    while (source.charAt(0) === '/') source = source.substring(1);
    var components = source.split('/');
    var result = [];
    for (var index = 0; index < components.length; ++index) {
      var component = components[index];
      if (!component || component === '.') continue;
      if (component === '..') throw new Error('Browser file paths must not contain parent components.');
      if (component.indexOf(':') !== -1 || /[\u0000-\u001f\u007f]/.test(component)) {
        throw new Error('Browser file paths contain an unsupported character.');
      }
      result.push(component);
    }
    if (!result.length) throw new Error('Browser file path must not be empty.');
    return result.join('/');
  }

  function chooseHostDirectoryFiles(options) {
    options = options || {};
    return new Promise(function(resolve) {
      var input = document.createElement('input');
      input.type = 'file';
      input.multiple = true;
      input.webkitdirectory = true;
      input.setAttribute('webkitdirectory', '');
      if (options.accept) input.accept = options.accept;
      input.style.position = 'fixed';
      input.style.left = '-10000px';
      input.style.top = '-10000px';
      input.setAttribute('aria-hidden', 'true');

      var settled = false;
      var focusTimer = 0;
      function onWindowFocus() {
        focusTimer = window.setTimeout(function() {
          if (!settled && (!input.files || input.files.length === 0)) finish([]);
        }, 350);
      }
      function finish(files) {
        if (settled) return;
        settled = true;
        if (focusTimer) window.clearTimeout(focusTimer);
        window.removeEventListener('focus', onWindowFocus);
        if (input.parentNode) input.parentNode.removeChild(input);
        resolve(files || []);
      }
      input.addEventListener('change', function() {
        finish(Array.prototype.slice.call(input.files || []));
      }, { once: true });
      input.addEventListener('cancel', function() { finish([]); }, { once: true });
      window.addEventListener('focus', onWindowFocus);
      document.body.appendChild(input);
      try { input.click(); }
      catch (error) {
        console.error('[NeoTools] Unable to open the browser directory picker:', error);
        finish([]);
      }
    });
  }

  async function retainFileEntries(displayName, sources, extensions) {
    var sessionId = nextRetainedFileSetId();
    var session = {
      id: sessionId,
      displayName: displayName || 'selected files',
      active: true,
      files: new Map()
    };
    var metadata = [];
    for (var index = 0; index < sources.length; ++index) {
      var source = sources[index];
      var relativePath = normalizeRetainedRelativePath(source.relativePath);
      var leaf = relativePath.substring(relativePath.lastIndexOf('/') + 1);
      if (!retainedFileMatches(leaf, extensions)) continue;
      var file = source.file || (source.handle ? await source.handle.getFile() : null);
      if (!file) continue;
      if (!Number.isSafeInteger(file.size) || file.size < 0) {
        throw new Error('A selected browser file is too large to address safely.');
      }
      if (metadata.length >= 0xFFFFFFFE) {
        throw new Error('The selected browser file set contains too many files.');
      }
      // IDs are session-local. Retain the immutable File snapshot even when a
      // FileSystemFileHandle is available so every header/table/payload read
      // observes one consistent archive version without loading its bytes.
      var fileId = metadata.length + 1;
      var entry = {
        id: fileId,
        relativePath: relativePath,
        file: file,
        handle: source.handle || null,
        size: file.size,
        lastModified: Number(file.lastModified || 0),
        active: true
      };
      session.files.set(fileId, entry);
      metadata.push({ id: fileId, path: relativePath, size: file.size });
    }
    if (!metadata.length) throw new Error('No matching files were selected.');
    retainedFileSets.set(sessionId, session);
    return { session: session, metadata: metadata };
  }

  async function chooseRetainedFiles(options) {
    options = options || {};
    var selected = await chooseHostFiles(options);
    if (!selected.length) return null;
    var sources = [];
    for (var index = 0; index < selected.length; ++index) {
      var entry = selected[index] || {};
      var file = entry.file || entry;
      sources.push({
        relativePath: entry.relativePath || file.name,
        file: file,
        handle: entry.handle || null
      });
    }
    return retainFileEntries('selected files', sources,
      parseRetainedAcceptExtensions(options.accept));
  }

  async function collectRetainedDirectoryHandles(directoryHandle, prefix, sources, state, depth, extensions) {
    if (depth > 32) throw new Error('The selected directory is nested too deeply.');
    for await (var pair of directoryHandle.entries()) {
      state.entries += 1;
      if (state.entries > 250000) throw new Error('The selected directory contains too many entries to scan safely.');
      var name = pair[0];
      var handle = pair[1];
      if (!name || /[\u0000-\u001f\u007f]/.test(name)) continue;
      var relativePath = prefix ? prefix + '/' + name : name;
      if (handle.kind === 'directory') {
        await collectRetainedDirectoryHandles(handle, relativePath, sources, state, depth + 1, extensions);
      } else if (handle.kind === 'file' && retainedFileMatches(name, extensions)) {
        sources.push({ relativePath: relativePath, file: null, handle: handle });
      }
    }
  }

  async function chooseRetainedDirectory(options) {
    options = options || {};
    var extensions = parseRetainedAcceptExtensions(options.accept);
    if (typeof window !== 'undefined' && window.isSecureContext === true &&
        typeof window.showDirectoryPicker === 'function') {
      var handle = null;
      try {
        handle = await window.showDirectoryPicker({ mode: 'read' });
      } catch (error) {
        if (error && error.name === 'AbortError') return null;
        throw error;
      }
      var sources = [];
      await collectRetainedDirectoryHandles(handle, '', sources, { entries: 0 }, 0, extensions);
      return retainFileEntries(handle.name || 'selected directory', sources, extensions);
    }

    var files = await chooseHostDirectoryFiles(options);
    if (!files.length) return null;
    var fallbackSources = [];
    var commonRoot = '';
    if (files[0].webkitRelativePath) {
      commonRoot = String(files[0].webkitRelativePath).replace(/\\/g, '/').split('/')[0] || '';
    }
    for (var index = 0; index < files.length; ++index) {
      var relative = files[index].webkitRelativePath || files[index].name;
      relative = String(relative).replace(/\\/g, '/');
      if (commonRoot && relative.indexOf(commonRoot + '/') === 0) {
        relative = relative.substring(commonRoot.length + 1);
      }
      fallbackSources.push({ relativePath: relative, file: files[index], handle: null });
    }
    return retainFileEntries(commonRoot || 'selected directory', fallbackSources, extensions);
  }

  function retainedMetadataPayload(metadata) {
    var lines = [];
    for (var index = 0; index < metadata.length; ++index) {
      var item = metadata[index];
      lines.push(String(item.id) + '\t' + String(item.size) + '\t' + encodeURIComponent(item.path));
    }
    return lines.join('\n');
  }

  function completeRetainedFileSetRequest(requestId, result, error) {
    result = result || null;
    window.setTimeout(function() {
      var sessionId = result && result.session ? result.session.id : 0;
      var displayName = result && result.session ? result.session.displayName : '';
      var payload = result ? retainedMetadataPayload(result.metadata || []) : '';
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Promise.resolve(Module.ccall(
          'neo_browser_retained_file_set_completed',
          null,
          ['number', 'number', 'string', 'string', 'string'],
          [requestId, sessionId, displayName, payload, error || ''],
          { async: true })).catch(function(callbackError) {
            if (sessionId && Module.neoToolsBrowserFiles &&
                Module.neoToolsBrowserFiles.releaseRetainedFileSet) {
              Module.neoToolsBrowserFiles.releaseRetainedFileSet(sessionId);
            }
            console.error('[NeoTools] Retained browser-file completion failed:', callbackError);
          });
      } catch (callbackError) {
        if (sessionId && Module.neoToolsBrowserFiles &&
            Module.neoToolsBrowserFiles.releaseRetainedFileSet) {
          Module.neoToolsBrowserFiles.releaseRetainedFileSet(sessionId);
        }
        console.error('[NeoTools] Retained browser-file completion failed:', callbackError);
      }
    }, 0);
  }

  function getRetainedSession(sessionId) {
    var session = retainedFileSets.get(Number(sessionId));
    if (!session || !session.active) throw new Error('The browser archive session is no longer active.');
    return session;
  }

  async function getRetainedFile(sessionId, fileId) {
    var session = getRetainedSession(sessionId);
    var entry = session.files.get(Number(fileId));
    if (!entry || !entry.active) throw new Error('The selected browser file is no longer available.');
    var file = entry.file || (entry.handle ? await entry.handle.getFile() : null);
    if (!session.active || !entry.active) {
      throw new Error('The browser archive file was replaced.');
    }
    if (!file || file.size !== entry.size || Number(file.lastModified || 0) !== entry.lastModified) {
      throw new Error('A selected archive file changed after it was scanned. Scan the directory again.');
    }
    return { session: session, entry: entry, file: file };
  }

  async function readRetainedFileRange(sessionId, fileId, offset, length, destination) {
    var resolved = await getRetainedFile(sessionId, fileId);
    var begin = Number(offset);
    var count = Number(length);
    var pointer = Number(destination);
    if (!Number.isSafeInteger(begin) || !Number.isSafeInteger(count) || begin < 0 || count < 0) {
      throw new Error('Invalid retained-file byte range.');
    }
    if (begin > resolved.file.size || count > resolved.file.size - begin) {
      throw new Error('Retained-file byte range extends beyond the selected file.');
    }
    var bytes = new Uint8Array(await resolved.file.slice(begin, begin + count).arrayBuffer());
    if (!resolved.session.active || !resolved.entry.active) {
      throw new Error('The browser archive file was replaced.');
    }
    if (bytes.length !== count) throw new Error('The browser returned an incomplete file range.');
    if (count) HEAPU8.set(bytes, pointer);
  }

  function parseRetainedExportManifest(payload) {
    var lines = String(payload || '').split('\n');
    var entries = [];
    var outputNames = Object.create(null);
    for (var index = 0; index < lines.length; ++index) {
      if (!lines[index]) continue;
      var fields = lines[index].split('\t');
      if (fields.length !== 5) throw new Error('Invalid retained export manifest.');
      var entry = {
        sessionId: Number(fields[0]),
        fileId: Number(fields[1]),
        offset: Number(fields[2]),
        size: Number(fields[3]),
        outputPath: normalizePackageRelativePath(decodeURIComponent(fields[4]), false)
      };
      if (!Number.isInteger(entry.sessionId) || entry.sessionId <= 0 ||
          !Number.isInteger(entry.fileId) || entry.fileId <= 0 ||
          !Number.isSafeInteger(entry.offset) || entry.offset < 0 ||
          !Number.isSafeInteger(entry.size) || entry.size < 0) {
        throw new Error('Invalid retained export entry.');
      }
      var outputKey = entry.outputPath.toLowerCase();
      if (outputNames[outputKey]) throw new Error('Duplicate export path: ' + entry.outputPath);
      outputNames[outputKey] = true;
      entries.push(entry);
    }
    if (!entries.length) throw new Error('No retained-file ranges were selected for export.');
    return entries;
  }

  function completeRetainedExportRequest(requestId, result, error) {
    result = result || {};
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_retained_export_completed',
          null,
          ['number', 'number', 'number', 'string', 'number', 'string'],
          [requestId,
           result.disposition || 0,
           result.filesWritten || 0,
           String(result.bytesWritten || 0),
           result.usedDirectory ? 1 : 0,
           error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Retained-file export completion failed:', callbackError);
      }
    }, 0);
  }

  function retainedDirectoryWriteSupported() {
    return typeof window !== 'undefined' && window.isSecureContext === true &&
      typeof window.showDirectoryPicker === 'function';
  }

  function assertRetainedSourceActive(source) {
    if (!source.session.active || !source.entry.active) {
      throw new Error('The browser archive file was replaced.');
    }
  }

  async function writeBlobInChunks(writable, blob, source) {
    var chunkSize = 4 * 1024 * 1024;
    for (var offset = 0; offset < blob.size; offset += chunkSize) {
      assertRetainedSourceActive(source);
      await writable.write(blob.slice(offset, Math.min(blob.size, offset + chunkSize)));
      assertRetainedSourceActive(source);
    }
  }

  function assertRetainedExportEntriesActive(entries) {
    var checked = Object.create(null);
    for (var index = 0; index < entries.length; ++index) {
      var sessionId = Number(entries[index].sessionId);
      var fileId = Number(entries[index].fileId);
      var key = String(sessionId) + ':' + String(fileId);
      if (checked[key]) continue;
      var session = getRetainedSession(sessionId);
      var entry = session.files.get(fileId);
      if (!entry || !entry.active) {
        throw new Error('A browser archive file selected for export was replaced.');
      }
      checked[key] = true;
    }
  }

  async function uniqueRetainedDestination(root, relativePath, createdDirectories) {
    var normalized = normalizePackageRelativePath(relativePath, false);
    var resolved = await resolvePackageFile(root, normalized, true, createdDirectories);
    if (!resolved.exists) return resolved;

    var slash = normalized.lastIndexOf('/');
    var parent = slash >= 0 ? normalized.substring(0, slash + 1) : '';
    var leaf = slash >= 0 ? normalized.substring(slash + 1) : normalized;
    var dot = leaf.lastIndexOf('.');
    var stem = dot > 0 ? leaf.substring(0, dot) : leaf;
    var extension = dot > 0 ? leaf.substring(dot) : '';
    for (var ordinal = 2; ordinal < 1000000; ++ordinal) {
      var candidate = parent + stem + '__' + ordinal + extension;
      resolved = await resolvePackageFile(root, candidate, true, createdDirectories);
      if (!resolved.exists) return resolved;
    }
    throw new Error('Unable to choose a collision-free browser extraction path for ' + normalized + '.');
  }

  async function exportRetainedDirectory(entries) {
    if (!retainedDirectoryWriteSupported()) {
      throw new Error('Writable directory selection is unavailable in this browser. Use Save as ZIP instead.');
    }
    assertRetainedExportEntriesActive(entries);
    var root = null;
    try {
      root = await window.showDirectoryPicker({ mode: 'readwrite' });
    } catch (error) {
      if (error && error.name === 'AbortError') return { disposition: 0 };
      throw error;
    }
    assertRetainedExportEntriesActive(entries);
    await ensurePackagePermission(root);
    assertRetainedExportEntriesActive(entries);
    var bytesWritten = 0;
    var filesWritten = 0;
    var created = [];
    var createdDirectories = [];
    try {
      for (var index = 0; index < entries.length; ++index) {
        var item = entries[index];
        var source = await getRetainedFile(item.sessionId, item.fileId);
        if (item.offset > source.file.size || item.size > source.file.size - item.offset) {
          throw new Error('Export range extends beyond ' + source.entry.relativePath + '.');
        }
        var destination = await uniqueRetainedDestination(
          root, item.outputPath, createdDirectories);
        assertRetainedSourceActive(source);
        var handle = await destination.parentHandle.getFileHandle(
          destination.requestedName, { create: true });
        created.push({ parentHandle: destination.parentHandle, name: destination.requestedName });
        assertRetainedSourceActive(source);
        var writable = null;
        try {
          writable = await handle.createWritable({ keepExistingData: false });
          assertRetainedSourceActive(source);
          await writeBlobInChunks(
            writable, source.file.slice(item.offset, item.offset + item.size), source);
          assertRetainedSourceActive(source);
          await writable.close();
          writable = null;
          assertRetainedSourceActive(source);
        } catch (error) {
          if (writable) { try { await writable.abort(); } catch (_) {} }
          throw error;
        }
        filesWritten += 1;
        bytesWritten += item.size;
      }
      assertRetainedExportEntriesActive(entries);
      return { disposition: 1, filesWritten: filesWritten, bytesWritten: bytesWritten, usedDirectory: true };
    } catch (error) {
      var rollbackFailures = 0;
      for (var createdIndex = created.length - 1; createdIndex >= 0; --createdIndex) {
        try {
          await created[createdIndex].parentHandle.removeEntry(created[createdIndex].name);
        } catch (_) {
          rollbackFailures += 1;
        }
      }
      for (var directoryIndex = createdDirectories.length - 1;
           directoryIndex >= 0; --directoryIndex) {
        try {
          await createdDirectories[directoryIndex].parentHandle.removeEntry(
            createdDirectories[directoryIndex].name);
        } catch (directoryError) {
          if (!directoryError || directoryError.name !== 'NotFoundError') {
            rollbackFailures += 1;
          }
        }
      }
      if (rollbackFailures) {
        var baseMessage = error && error.message ? error.message : String(error || 'Browser extraction failed.');
        throw new Error(baseMessage + ' Rollback could not remove ' + rollbackFailures +
          ' partially extracted file or director' + (rollbackFailures === 1 ? 'y.' : 'ies.'));
      }
      throw error;
    }
  }

  var retainedCrcTable = null;
  function getRetainedCrcTable() {
    if (retainedCrcTable) return retainedCrcTable;
    retainedCrcTable = new Uint32Array(256);
    for (var index = 0; index < 256; ++index) {
      var value = index;
      for (var bit = 0; bit < 8; ++bit) {
        value = (value & 1) ? (0xEDB88320 ^ (value >>> 1)) : (value >>> 1);
      }
      retainedCrcTable[index] = value >>> 0;
    }
    return retainedCrcTable;
  }

  function updateRetainedCrc(crc, bytes) {
    var table = getRetainedCrcTable();
    var value = crc >>> 0;
    for (var index = 0; index < bytes.length; ++index) {
      value = (table[(value ^ bytes[index]) & 0xFF] ^ (value >>> 8)) >>> 0;
    }
    return value >>> 0;
  }

  function makeBinary(size) {
    return { bytes: new Uint8Array(size), view: null, offset: 0 };
  }
  function binaryView(binary) {
    if (!binary.view) binary.view = new DataView(binary.bytes.buffer);
    return binary.view;
  }
  function putU16(binary, value) {
    binaryView(binary).setUint16(binary.offset, value & 0xFFFF, true);
    binary.offset += 2;
  }
  function putU32(binary, value) {
    binaryView(binary).setUint32(binary.offset, value >>> 0, true);
    binary.offset += 4;
  }
  function putBytes(binary, bytes) {
    binary.bytes.set(bytes, binary.offset);
    binary.offset += bytes.length;
  }

  function zipLocalHeader(nameBytes) {
    var binary = makeBinary(30 + nameBytes.length);
    putU32(binary, 0x04034B50);
    putU16(binary, 20);
    putU16(binary, 0x0808);
    putU16(binary, 0);
    putU16(binary, 0);
    putU16(binary, 0x0021);
    putU32(binary, 0);
    putU32(binary, 0);
    putU32(binary, 0);
    putU16(binary, nameBytes.length);
    putU16(binary, 0);
    putBytes(binary, nameBytes);
    return binary.bytes;
  }

  function zipDataDescriptor(crc, size) {
    var binary = makeBinary(16);
    putU32(binary, 0x08074B50);
    putU32(binary, crc);
    putU32(binary, size);
    putU32(binary, size);
    return binary.bytes;
  }

  function zipCentralHeader(entry) {
    var binary = makeBinary(46 + entry.nameBytes.length);
    putU32(binary, 0x02014B50);
    putU16(binary, 20);
    putU16(binary, 20);
    putU16(binary, 0x0808);
    putU16(binary, 0);
    putU16(binary, 0);
    putU16(binary, 0x0021);
    putU32(binary, entry.crc);
    putU32(binary, entry.size);
    putU32(binary, entry.size);
    putU16(binary, entry.nameBytes.length);
    putU16(binary, 0);
    putU16(binary, 0);
    putU16(binary, 0);
    putU16(binary, 0);
    putU32(binary, 0);
    putU32(binary, entry.localOffset);
    putBytes(binary, entry.nameBytes);
    return binary.bytes;
  }

  function zipEndOfDirectory(count, centralSize, centralOffset) {
    var binary = makeBinary(22);
    putU32(binary, 0x06054B50);
    putU16(binary, 0);
    putU16(binary, 0);
    putU16(binary, count);
    putU16(binary, count);
    putU32(binary, centralSize);
    putU32(binary, centralOffset);
    putU16(binary, 0);
    return binary.bytes;
  }

  function cooperativeBrowserYield() {
    return new Promise(function(resolve) { window.setTimeout(resolve, 0); });
  }

  async function createRetainedZipSink(defaultName) {
    var safeName = appendDefaultBrowserExtension(defaultName || 'resources.zip', '.zip');
    if (typeof window !== 'undefined' && window.isSecureContext === true &&
        typeof window.showSaveFilePicker === 'function') {
      var handle = null;
      try {
        handle = await window.showSaveFilePicker({
          suggestedName: safeName,
          types: [{ description: 'ZIP archive', accept: { 'application/zip': ['.zip'] } }]
        });
      } catch (error) {
        if (error && error.name === 'AbortError') return null;
        throw error;
      }
      var writable = await handle.createWritable({ keepExistingData: false });
      return {
        position: 0,
        writable: writable,
        parts: null,
        async writeBytes(bytes) { await writable.write(bytes); this.position += bytes.byteLength; },
        async writeBlob(blob) { await writable.write(blob); this.position += blob.size; },
        async finish() { await writable.close(); this.writable = null; return { disposition: 1 }; },
        async abort() { if (this.writable) { try { await this.writable.abort(); } catch (_) {} this.writable = null; } }
      };
    }
    return {
      position: 0,
      writable: null,
      parts: [],
      async writeBytes(bytes) { this.parts.push(bytes); this.position += bytes.byteLength; },
      async writeBlob(blob) { this.parts.push(blob); this.position += blob.size; },
      async finish() {
        var blob = new Blob(this.parts, { type: 'application/zip' });
        this.parts = [];
        queueBrowserDownloadBlob(blob, safeName);
        return { disposition: 2 };
      },
      async abort() { this.parts = []; }
    };
  }

  async function exportRetainedZip(entries, defaultName) {
    if (entries.length > 0xFFFF) throw new Error('ZIP output exceeds the classic ZIP entry limit.');
    assertRetainedExportEntriesActive(entries);
    var encoder = new TextEncoder();
    var encodedNames = [];
    var projectedSize = 22;
    for (var preflightIndex = 0; preflightIndex < entries.length; ++preflightIndex) {
      var preflightItem = entries[preflightIndex];
      if (preflightItem.size > 0xFFFFFFFF) {
        throw new Error('A ZIP entry exceeds the classic ZIP size limit.');
      }
      var encodedName = encoder.encode(preflightItem.outputPath);
      if (!encodedName.length || encodedName.length > 0xFFFF) {
        throw new Error('ZIP entry name is invalid or too long.');
      }
      encodedNames.push(encodedName);
      projectedSize += 30 + encodedName.length + preflightItem.size + 16;
      projectedSize += 46 + encodedName.length;
      if (projectedSize > 0xFFFFFFFF) {
        throw new Error('ZIP output exceeds the classic ZIP size limit.');
      }
    }

    var sink = await createRetainedZipSink(defaultName);
    if (!sink) return { disposition: 0 };
    if (!sink.writable && projectedSize > MAX_FALLBACK_ZIP_BYTES) {
      await sink.abort();
      throw new Error('Fallback ZIP output exceeds the configured browser limit. Use a browser with streaming save support or extract fewer resources.');
    }
    assertRetainedExportEntriesActive(entries);
    var central = [];
    var bytesWritten = 0;
    try {
      for (var index = 0; index < entries.length; ++index) {
        var item = entries[index];
        if (sink.position > 0xFFFFFFFF) throw new Error('ZIP output exceeds the classic ZIP size limit.');
        var source = await getRetainedFile(item.sessionId, item.fileId);
        if (item.offset > source.file.size || item.size > source.file.size - item.offset) {
          throw new Error('Export range extends beyond ' + source.entry.relativePath + '.');
        }
        var nameBytes = encodedNames[index];
        var localOffset = sink.position;
        await sink.writeBytes(zipLocalHeader(nameBytes));
        assertRetainedSourceActive(source);

        var crc = 0xFFFFFFFF;
        var chunkSize = 4 * 1024 * 1024;
        if (sink.writable) {
          for (var offset = 0; offset < item.size; offset += chunkSize) {
            assertRetainedSourceActive(source);
            var chunkBlob = source.file.slice(item.offset + offset,
              item.offset + Math.min(item.size, offset + chunkSize));
            var chunk = new Uint8Array(await chunkBlob.arrayBuffer());
            assertRetainedSourceActive(source);
            crc = updateRetainedCrc(crc, chunk);
            await sink.writeBytes(chunk);
            assertRetainedSourceActive(source);
            await cooperativeBrowserYield();
          }
        } else {
          for (var crcOffset = 0; crcOffset < item.size; crcOffset += chunkSize) {
            assertRetainedSourceActive(source);
            var crcBlob = source.file.slice(item.offset + crcOffset,
              item.offset + Math.min(item.size, crcOffset + chunkSize));
            var crcBytes = new Uint8Array(await crcBlob.arrayBuffer());
            assertRetainedSourceActive(source);
            crc = updateRetainedCrc(crc, crcBytes);
            await cooperativeBrowserYield();
          }
          assertRetainedSourceActive(source);
          await sink.writeBlob(source.file.slice(item.offset, item.offset + item.size));
          assertRetainedSourceActive(source);
        }
        crc = (crc ^ 0xFFFFFFFF) >>> 0;
        await sink.writeBytes(zipDataDescriptor(crc, item.size));
        assertRetainedSourceActive(source);
        central.push({ nameBytes: nameBytes, crc: crc, size: item.size, localOffset: localOffset });
        bytesWritten += item.size;
      }

      assertRetainedExportEntriesActive(entries);
      var centralOffset = sink.position;
      for (var centralIndex = 0; centralIndex < central.length; ++centralIndex) {
        await sink.writeBytes(zipCentralHeader(central[centralIndex]));
        assertRetainedExportEntriesActive(entries);
      }
      var centralSize = sink.position - centralOffset;
      if (sink.position > 0xFFFFFFFF || centralOffset > 0xFFFFFFFF || centralSize > 0xFFFFFFFF) {
        throw new Error('ZIP central directory exceeds the classic ZIP size limit.');
      }
      await sink.writeBytes(zipEndOfDirectory(central.length, centralSize, centralOffset));
      assertRetainedExportEntriesActive(entries);
      var completion = await sink.finish();
      return {
        disposition: completion.disposition,
        filesWritten: entries.length,
        bytesWritten: bytesWritten,
        usedDirectory: false
      };
    } catch (error) {
      await sink.abort();
      throw error;
    }
  }

  async function exportRetainedDirect(entries, defaultName) {
    if (entries.length !== 1) throw new Error('Direct download requires exactly one selected range.');
    assertRetainedExportEntriesActive(entries);
    var item = entries[0];
    var source = await getRetainedFile(item.sessionId, item.fileId);
    if (item.offset > source.file.size || item.size > source.file.size - item.offset) {
      throw new Error('Export range extends beyond ' + source.entry.relativePath + '.');
    }
    assertRetainedSourceActive(source);
    var name = safeBrowserFileName(defaultName || item.outputPath, 'download.bin');
    queueBrowserDownloadBlob(source.file.slice(item.offset, item.offset + item.size), name);
    return { disposition: 2, filesWritten: 1, bytesWritten: item.size, usedDirectory: false };
  }

  async function exportRetainedEntries(mode, defaultName, payload) {
    var entries = parseRetainedExportManifest(payload);
    if (Number(mode) === 0) return exportRetainedDirect(entries, defaultName);
    if (Number(mode) === 2) return exportRetainedDirectory(entries);
    return exportRetainedZip(entries, defaultName);
  }


  function completeOpenFilesRequest(requestId, paths, error) {
    // The wx DOM port enters C++ menu/control handlers through a synchronous
    // ccall. Complete in a later browser task so the original wx event has
    // fully unwound before entering WebAssembly again.
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_open_files_completed',
          null,
          ['number', 'string', 'string'],
          [requestId, (paths || []).join('\n'), error || '']);
      } catch (callbackError) {
        releaseLegacyImportedPaths(paths || []);
        console.error('[NeoTools] Browser file completion callback failed:', callbackError);
      }
    }, 0);
  }


  function completePackageDirectoryRequest(requestId, sessionId, displayName, iniPaths, error) {
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_package_directory_completed',
          null,
          ['number', 'number', 'string', 'string', 'string'],
          [requestId, sessionId || 0, displayName || '', (iniPaths || []).join('\n'), error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser package-directory callback failed:', callbackError);
      }
    }, 0);
  }

  function completePackageWorkspaceRequest(requestId, result, error) {
    result = result || {};
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_package_workspace_completed',
          null,
          ['number', 'number', 'string', 'string', 'string', 'number', 'string'],
          [requestId,
           result.sessionId || 0,
           result.workspaceRoot || '',
           result.iniPath || '',
           result.relativeIniPath || '',
           result.iniExisted ? 1 : 0,
           error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser package-workspace callback failed:', callbackError);
      }
    }, 0);
  }

  function completePackageCommitRequest(requestId, result, error) {
    result = result || {};
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_package_commit_completed',
          null,
          ['number', 'number', 'number', 'number', 'string'],
          [requestId,
           result.filesWritten || 0,
           result.filesReused || 0,
           result.iniChanged ? 1 : 0,
           error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser package-commit callback failed:', callbackError);
      }
    }, 0);
  }

  function packageDirectorySupported() {
    return typeof window !== 'undefined' &&
      window.isSecureContext === true &&
      typeof window.showDirectoryPicker === 'function';
  }

  function normalizePackageRelativePath(value, requireIni) {
    var source = String(value || '').replace(/\\/g, '/').trim();
    if (!source) throw new Error('Package-relative path must not be empty.');
    if (source.charAt(0) === '/' || /^[A-Za-z]:/.test(source)) {
      throw new Error('Package paths must be relative to the selected installer folder.');
    }
    var components = source.split('/');
    for (var index = 0; index < components.length; ++index) {
      var component = components[index];
      if (!component || component === '.' || component === '..') {
        throw new Error('Package paths must not contain empty, current, or parent components.');
      }
      if (component.indexOf(':') !== -1) {
        throw new Error('Package paths must not contain a colon.');
      }
      if (/[\u0000-\u001f\u007f]/.test(component)) {
        throw new Error('Package paths must not contain control characters.');
      }
    }
    var result = components.join('/');
    if (requireIni) {
      var leaf = components[components.length - 1];
      var dot = leaf.lastIndexOf('.');
      if (dot < 0) result += '.ini';
      else if (leaf.substring(dot).toLowerCase() !== '.ini') {
        throw new Error('The selected installer configuration must use the .ini extension.');
      }
    }
    return result;
  }

  function parsePackagePathPayload(payload) {
    var values = String(payload || '').split('\n');
    var result = [];
    var seen = Object.create(null);
    for (var index = 0; index < values.length; ++index) {
      if (!values[index]) continue;
      var normalized = normalizePackageRelativePath(values[index], false);
      var key = normalized.toLowerCase();
      if (seen[key]) continue;
      seen[key] = true;
      result.push(normalized);
    }
    return result;
  }

  function bytesEqual(left, right) {
    if (!left || !right || left.length !== right.length) return false;
    for (var index = 0; index < left.length; ++index) {
      if (left[index] !== right[index]) return false;
    }
    return true;
  }

  function copyBytes(bytes) {
    var copy = new Uint8Array(bytes ? bytes.length : 0);
    if (bytes && bytes.length) copy.set(bytes);
    return copy;
  }

  async function ensurePackagePermission(handle) {
    if (!handle) throw new Error('The selected installer folder is unavailable.');
    var options = { mode: 'readwrite' };
    if (typeof handle.queryPermission === 'function') {
      var state = await handle.queryPermission(options);
      if (state === 'granted') return;
    }
    if (typeof handle.requestPermission === 'function') {
      var requested = await handle.requestPermission(options);
      if (requested === 'granted') return;
    }
    throw new Error('Read/write permission for the selected installer folder was not granted.');
  }

  async function collectPackageIniPaths(directoryHandle, prefix, result, state, depth) {
    if (depth > 20) throw new Error('The selected installer folder is nested too deeply.');
    for await (var entry of directoryHandle.entries()) {
      state.entries += 1;
      if (state.entries > 20000) {
        throw new Error('The selected installer folder contains too many entries to scan safely.');
      }
      var name = entry[0];
      var handle = entry[1];
      if (/[\u0000-\u001f\u007f]/.test(name)) continue;
      var relative = prefix ? prefix + '/' + name : name;
      if (handle.kind === 'file') {
        if (/\.ini$/i.test(name)) result.push(relative);
      } else if (handle.kind === 'directory') {
        await collectPackageIniPaths(handle, relative, result, state, depth + 1);
      }
    }
  }

  async function findPackageChild(directoryHandle, requestedName, expectedKind) {
    var exact = null;
    try {
      exact = expectedKind === 'directory'
        ? await directoryHandle.getDirectoryHandle(requestedName, { create: false })
        : await directoryHandle.getFileHandle(requestedName, { create: false });
    } catch (error) {
      if (!error || (error.name !== 'NotFoundError' && error.name !== 'TypeMismatchError')) throw error;
    }
    if (exact) return { name: requestedName, handle: exact };

    var lower = requestedName.toLowerCase();
    var match = null;
    for await (var entry of directoryHandle.entries()) {
      if (entry[0].toLowerCase() !== lower || entry[1].kind !== expectedKind) continue;
      if (match) {
        throw new Error('The package directory contains ambiguous case-insensitive names for ' + requestedName + '.');
      }
      match = { name: entry[0], handle: entry[1] };
    }
    return match;
  }

  async function resolvePackageFile(rootHandle, relativePath, createParents, createdDirectories) {
    var normalized = normalizePackageRelativePath(relativePath, false);
    var components = normalized.split('/');
    var directory = rootHandle;
    var actualComponents = [];
    for (var index = 0; index + 1 < components.length; ++index) {
      var foundDirectory = await findPackageChild(directory, components[index], 'directory');
      if (foundDirectory) {
        directory = foundDirectory.handle;
        actualComponents.push(foundDirectory.name);
      } else if (createParents) {
        var parentHandle = directory;
        directory = await parentHandle.getDirectoryHandle(components[index], { create: true });
        if (createdDirectories) {
          createdDirectories.push({ parentHandle: parentHandle, name: components[index] });
        }
        actualComponents.push(components[index]);
      } else {
        return {
          exists: false,
          parentHandle: null,
          requestedName: components[components.length - 1],
          actualPath: normalized,
          relativePath: normalized
        };
      }
    }

    var requestedName = components[components.length - 1];
    var foundFile = await findPackageChild(directory, requestedName, 'file');
    if (!foundFile) {
      return {
        exists: false,
        parentHandle: directory,
        requestedName: requestedName,
        actualPath: actualComponents.concat([requestedName]).join('/'),
        relativePath: normalized
      };
    }
    return {
      exists: true,
      parentHandle: directory,
      fileHandle: foundFile.handle,
      requestedName: requestedName,
      actualName: foundFile.name,
      actualPath: actualComponents.concat([foundFile.name]).join('/'),
      relativePath: normalized
    };
  }

  async function readPackageFile(rootHandle, relativePath) {
    var resolved = await resolvePackageFile(rootHandle, relativePath, false);
    if (!resolved.exists) {
      resolved.bytes = null;
      return resolved;
    }
    var file = await resolved.fileHandle.getFile();
    resolved.bytes = new Uint8Array(await file.arrayBuffer());
    return resolved;
  }

  function writePackageWorkspaceFile(workspaceRoot, relativePath, bytes) {
    var normalized = normalizePackageRelativePath(relativePath, false);
    var fullPath = workspaceRoot + '/' + normalized;
    var slash = fullPath.lastIndexOf('/');
    if (slash > 0) FS.mkdirTree(fullPath.substring(0, slash));
    FS.writeFile(fullPath, bytes, { canOwn: true });
    return fullPath;
  }

  async function writePackageHostFile(rootHandle, relativePath, bytes) {
    var resolved = await resolvePackageFile(rootHandle, relativePath, true);
    var existed = resolved.exists;
    var handle = existed
      ? resolved.fileHandle
      : await resolved.parentHandle.getFileHandle(resolved.requestedName, { create: true });
    var writable = null;
    try {
      writable = await handle.createWritable();
      await writable.write(bytes);
      await writable.close();
    } catch (error) {
      if (writable) {
        try { await writable.abort(); } catch (_) {}
      }
      // getFileHandle({ create: true }) can create the directory entry before
      // createWritable(), write(), or close() succeeds. Remove that partial
      // entry immediately so the outer transaction can roll back cleanly.
      if (!existed && resolved.parentHandle) {
        try { await resolved.parentHandle.removeEntry(resolved.requestedName); } catch (_) {}
      }
      throw error;
    }
    return {
      created: !existed,
      parentHandle: resolved.parentHandle,
      name: existed ? resolved.actualName : resolved.requestedName
    };
  }

  async function rollbackPackageHostWrites(rootHandle, journal) {
    var failures = [];
    for (var index = journal.length - 1; index >= 0; --index) {
      var entry = journal[index];
      try {
        if (entry.existed) {
          // A later write failed after this destination had already been
          // replaced. Restore the exact bytes observed during preflight rather
          // than leaving the installer folder partially updated.
          await writePackageHostFile(
            rootHandle, entry.relativePath, entry.previousBytes);
          continue;
        }

        // The transaction created this file. Resolve it again so rollback also
        // works when the original write failed after creating the directory
        // entry but before returning its handle to the caller.
        var resolved = await resolvePackageFile(
          rootHandle, entry.relativePath, false);
        if (resolved.exists && resolved.parentHandle) {
          await resolved.parentHandle.removeEntry(
            resolved.actualName || resolved.requestedName);
        }
      } catch (error) {
        var message = error && error.message
          ? error.message
          : String(error || 'Unknown rollback error.');
        failures.push(entry.relativePath + ': ' + message);
        console.warn(
          '[NeoTools] Unable to roll back package file ' +
            entry.relativePath + ':',
          error);
      }
    }
    return failures;
  }

  async function choosePackageDirectory() {
    if (!packageDirectorySupported()) {
      throw new Error('Writable installer-folder selection requires a browser with the File System Access API.');
    }
    var handle = await window.showDirectoryPicker({
      id: 'neotools-tslpatcher-package',
      mode: 'readwrite'
    });
    await ensurePackagePermission(handle);
    var iniPaths = [];
    await collectPackageIniPaths(handle, '', iniPaths, { entries: 0 }, 0);
    iniPaths.sort(function(left, right) {
      return left.localeCompare(right, undefined, { sensitivity: 'base', numeric: true });
    });
    packageDirectorySequence += 1;
    var sessionId = packageDirectorySequence;
    packageDirectorySessions.set(sessionId, {
      handle: handle,
      displayName: handle.name || 'installer folder',
      transaction: null
    });
    return { sessionId: sessionId, displayName: handle.name || 'installer folder', iniPaths: iniPaths };
  }

  async function preparePackageWorkspace(sessionId, relativeIniPath, relativeFilesPayload) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    var session = packageDirectorySessions.get(Number(sessionId));
    if (!session) throw new Error('The selected installer-folder session has expired. Select it again.');
    await ensurePackagePermission(session.handle);

    var iniPath = normalizePackageRelativePath(relativeIniPath, true);
    var paths = parsePackagePathPayload(relativeFilesPayload);
    var iniKey = iniPath.toLowerCase();
    if (!paths.some(function(path) { return path.toLowerCase() === iniKey; })) paths.push(iniPath);

    var workspaceRoot = uniqueBrowserDirectory('package');
    var baselines = new Map();
    var seen = Object.create(null);
    var iniExisted = false;
    for (var index = 0; index < paths.length; ++index) {
      var normalized = normalizePackageRelativePath(paths[index], false);
      var key = normalized.toLowerCase();
      if (seen[key]) continue;
      seen[key] = true;
      var host = await readPackageFile(session.handle, normalized);
      var baseline = {
        relativePath: normalized,
        exists: !!host.exists,
        actualPath: host.actualPath || normalized,
        bytes: host.exists ? copyBytes(host.bytes) : null
      };
      baselines.set(key, baseline);
      if (host.exists) writePackageWorkspaceFile(workspaceRoot, normalized, host.bytes);
      if (key === iniKey) iniExisted = host.exists;
    }

    session.transaction = {
      workspaceRoot: workspaceRoot,
      relativeIniPath: iniPath,
      baselines: baselines
    };
    return {
      sessionId: Number(sessionId),
      workspaceRoot: workspaceRoot,
      iniPath: workspaceRoot + '/' + iniPath,
      relativeIniPath: iniPath,
      iniExisted: iniExisted
    };
  }

  async function commitPackageWorkspace(sessionId, workspaceRoot, relativeIniPath, relativeFilesPayload) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    var session = packageDirectorySessions.get(Number(sessionId));
    if (!session || !session.transaction) {
      throw new Error('The installer-package transaction has expired. Select the folder again.');
    }
    var transaction = session.transaction;
    // Detach the transaction immediately so imported host-file baselines are
    // released after this commit attempt, whether it succeeds or fails. A new
    // export always performs a fresh preflight before writing anything.
    session.transaction = null;
    var normalizedRoot = String(workspaceRoot || '').replace(/\/+$/, '');
    var iniPath = normalizePackageRelativePath(relativeIniPath, true);
    if (normalizedRoot !== transaction.workspaceRoot || iniPath !== transaction.relativeIniPath) {
      throw new Error('The installer-package transaction no longer matches the selected output.');
    }
    await ensurePackagePermission(session.handle);

    var paths = parsePackagePathPayload(relativeFilesPayload);
    var iniKey = iniPath.toLowerCase();
    if (!paths.some(function(path) { return path.toLowerCase() === iniKey; })) paths.push(iniPath);

    var plans = [];
    var seen = Object.create(null);
    for (var index = 0; index < paths.length; ++index) {
      var relative = normalizePackageRelativePath(paths[index], false);
      var key = relative.toLowerCase();
      if (seen[key]) continue;
      seen[key] = true;
      var baseline = transaction.baselines.get(key);
      if (!baseline) {
        throw new Error('The package output was not included in the verified preflight: ' + relative);
      }
      var virtualPath = normalizedRoot + '/' + relative;
      var generated;
      try {
        generated = FS.readFile(virtualPath, { encoding: 'binary' });
      } catch (error) {
        throw new Error('The generated package file is missing: ' + relative);
      }
      generated = copyBytes(generated);

      var current = await readPackageFile(session.handle, relative);
      if (baseline.exists) {
        if (!current.exists || !bytesEqual(current.bytes, baseline.bytes)) {
          throw new Error('The installer package changed while NeoTools was generating output: ' + relative);
        }
      } else if (current.exists) {
        throw new Error('A new package file appeared while NeoTools was generating output: ' + relative);
      }

      var isIni = key === iniKey;
      if (!isIni && current.exists && !bytesEqual(current.bytes, generated)) {
        throw new Error('The package already contains a different payload named ' + relative + '.');
      }
      plans.push({
        relativePath: relative,
        key: key,
        isIni: isIni,
        generated: generated,
        current: current
      });
    }

    plans.sort(function(left, right) {
      if (left.isIni === right.isIni) return left.relativePath.localeCompare(right.relativePath);
      return left.isIni ? 1 : -1;
    });

    var filesWritten = 0;
    var filesReused = 0;
    var iniChanged = false;
    var rollbackJournal = [];
    try {
      for (var planIndex = 0; planIndex < plans.length; ++planIndex) {
        var plan = plans[planIndex];
        if (plan.current.exists && bytesEqual(plan.current.bytes, plan.generated)) {
          filesReused += 1;
          continue;
        }
        // Journal the destination before starting the write. This also covers
        // a close() failure whose browser implementation may already have
        // replaced the destination before reporting the error.
        rollbackJournal.push({
          relativePath: plan.relativePath,
          existed: !!plan.current.exists,
          previousBytes: plan.current.exists
            ? copyBytes(plan.current.bytes)
            : null
        });
        await writePackageHostFile(
          session.handle, plan.relativePath, plan.generated);
        filesWritten += 1;
        if (plan.isIni) iniChanged = true;
      }
    } catch (error) {
      var rollbackFailures = await rollbackPackageHostWrites(
        session.handle, rollbackJournal);
      if (rollbackFailures.length) {
        var originalMessage = error && error.message
          ? error.message
          : String(error || 'Unknown installer-package commit error.');
        throw new Error(
          originalMessage + ' Rollback was incomplete: ' +
            rollbackFailures.join('; '));
      }
      throw error;
    }

    for (var updateIndex = 0; updateIndex < plans.length; ++updateIndex) {
      var updated = plans[updateIndex];
      transaction.baselines.set(updated.key, {
        relativePath: updated.relativePath,
        exists: true,
        actualPath: updated.relativePath,
        bytes: copyBytes(updated.generated)
      });
    }
    return {
      filesWritten: filesWritten,
      filesReused: filesReused,
      iniChanged: iniChanged
    };
  }



  var activeDownloadEntries = [];
  var activeDownloadBytes = 0;

  function completeDownloadRequest(requestId, disposition, error) {
    // As with file-open completion, leave the originating wx DOM event before
    // entering WebAssembly again. The callback updates status or displays an
    // error through wx's normal pending-event pump.
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_download_completed',
          null,
          ['number', 'number', 'string'],
          [requestId, disposition, error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser download completion callback failed:', callbackError);
      }
    }, 0);
  }

  function ensureDownloadHost() {
    var host = document.getElementById('neo-download-host');
    if (host) return host;

    host = document.createElement('div');
    host.id = 'neo-download-host';
    host.setAttribute('role', 'region');
    host.setAttribute('aria-label', 'Prepared downloads');
    host.setAttribute('aria-live', 'polite');
    host.style.position = 'fixed';
    host.style.left = '50%';
    host.style.top = '48px';
    host.style.transform = 'translateX(-50%)';
    host.style.zIndex = '2147483647';
    host.style.display = 'flex';
    host.style.flexDirection = 'column';
    host.style.alignItems = 'stretch';
    host.style.gap = '7px';
    host.style.width = 'min(620px, calc(100vw - 24px))';
    host.style.maxHeight = 'min(45vh, 360px)';
    host.style.overflowY = 'auto';
    host.style.padding = '9px';
    host.style.background = '#20242b';
    host.style.border = '1px solid #72a7e7';
    host.style.borderRadius = '8px';
    host.style.boxShadow = '0 10px 32px rgba(0,0,0,.42)';
    host.style.pointerEvents = 'auto';
    host.style.fontFamily = 'system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif';
    document.body.appendChild(host);
    return host;
  }

  function removeDownloadEntry(entry) {
    if (!entry) return;
    var index = activeDownloadEntries.indexOf(entry);
    if (index < 0) return;
    if (entry.timer) window.clearTimeout(entry.timer);
    if (entry.item && entry.item.parentNode) entry.item.parentNode.removeChild(entry.item);
    activeDownloadEntries.splice(index, 1);
    activeDownloadBytes = Math.max(0, activeDownloadBytes - Number(entry.size || 0));
    try { URL.revokeObjectURL(entry.url); } catch (_) {}
    var host = document.getElementById('neo-download-host');
    if (!activeDownloadEntries.length && host && host.parentNode) {
      host.parentNode.removeChild(host);
    }
  }

  function removeDownloadItem(item, url) {
    for (var index = activeDownloadEntries.length - 1; index >= 0; --index) {
      var entry = activeDownloadEntries[index];
      if (entry.item === item || entry.url === url) removeDownloadEntry(entry);
    }
  }

  function prunePreparedDownloads(requiredBytes) {
    if (!Number.isSafeInteger(requiredBytes) || requiredBytes < 0 ||
        requiredBytes > MAX_PREPARED_DOWNLOAD_BYTES) {
      throw new Error('Prepared download exceeds the configured browser retention limit.');
    }
    var now = Date.now();
    var ordered = activeDownloadEntries.slice().sort(function(left, right) {
      return left.createdAt - right.createdAt;
    });
    for (var index = 0; index < ordered.length; ++index) {
      if (now - ordered[index].createdAt >= MAX_PREPARED_DOWNLOAD_AGE_MS) {
        removeDownloadEntry(ordered[index]);
      }
    }
    ordered = activeDownloadEntries.slice().sort(function(left, right) {
      return left.createdAt - right.createdAt;
    });
    while (ordered.length &&
           (activeDownloadEntries.length >= MAX_PREPARED_DOWNLOADS ||
            activeDownloadBytes + requiredBytes > MAX_PREPARED_DOWNLOAD_BYTES)) {
      removeDownloadEntry(ordered.shift());
    }
    if (activeDownloadEntries.length >= MAX_PREPARED_DOWNLOADS ||
        activeDownloadBytes + requiredBytes > MAX_PREPARED_DOWNLOAD_BYTES) {
      throw new Error('Prepared downloads exceed the bounded browser retention budget.');
    }
  }

  function queueBrowserDownloadBlob(blob, name) {
    if (!blob || !Number.isSafeInteger(blob.size) || blob.size < 0) {
      throw new Error('Prepared download has an invalid size.');
    }
    prunePreparedDownloads(blob.size);
    var url = URL.createObjectURL(blob);

    var host = ensureDownloadHost();
    var item = document.createElement('div');
    item.style.display = 'grid';
    item.style.gridTemplateColumns = 'minmax(0,1fr) auto';
    item.style.alignItems = 'center';
    item.style.gap = '8px';
    item.style.padding = '7px';
    item.style.background = '#2b323c';
    item.style.border = '1px solid #596473';
    item.style.borderRadius = '6px';

    var content = document.createElement('div');
    content.style.minWidth = '0';

    var message = document.createElement('div');
    message.textContent = 'File ready';
    message.style.marginBottom = '5px';
    message.style.color = '#eef2f6';
    message.style.fontSize = '12px';
    message.style.fontWeight = '650';

    var anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = name;
    anchor.textContent = 'Download ' + name;
    anchor.title = 'Download ' + name;
    anchor.setAttribute('data-neo-download-name', name);
    anchor.style.display = 'block';
    anchor.style.width = '100%';
    anchor.style.padding = '8px 11px';
    anchor.style.border = '1px solid #8fc0ff';
    anchor.style.borderRadius = '5px';
    anchor.style.background = '#315f91';
    anchor.style.color = '#ffffff';
    anchor.style.fontWeight = '700';
    anchor.style.textAlign = 'center';
    anchor.style.textDecoration = 'none';
    anchor.style.whiteSpace = 'nowrap';
    anchor.style.overflow = 'hidden';
    anchor.style.textOverflow = 'ellipsis';
    anchor.style.cursor = 'pointer';
    anchor.addEventListener('click', function() {
      message.textContent = 'Download requested — the link remains available';
      anchor.textContent = 'Download again: ' + name;
    });

    var close = document.createElement('button');
    close.type = 'button';
    close.textContent = '×';
    close.title = 'Dismiss prepared download';
    close.setAttribute('aria-label', 'Dismiss prepared download ' + name);
    close.style.width = '30px';
    close.style.height = '30px';
    close.style.padding = '0';
    close.style.border = '1px solid #66717f';
    close.style.borderRadius = '5px';
    close.style.background = '#343c47';
    close.style.color = '#eef2f6';
    close.style.cursor = 'pointer';
    close.addEventListener('click', function(event) {
      event.preventDefault();
      event.stopPropagation();
      removeDownloadItem(item, url);
    });

    content.appendChild(message);
    content.appendChild(anchor);
    item.appendChild(content);
    item.appendChild(close);
    host.prepend(item);
    var entry = {
      item: item,
      url: url,
      anchor: anchor,
      size: blob.size,
      createdAt: Date.now(),
      timer: 0
    };
    entry.timer = window.setTimeout(function() {
      removeDownloadEntry(entry);
    }, MAX_PREPARED_DOWNLOAD_AGE_MS);
    activeDownloadEntries.push(entry);
    activeDownloadBytes += blob.size;
    // Do not synthesize a click and do not invoke a native save picker here.
    // The next click is a normal browser DOM activation on this visible link,
    // outside wxWidgets-WASM's synchronous event-dispatch interlock.
    try { anchor.focus({ preventScroll: true }); } catch (_) { try { anchor.focus(); } catch (_) {} }
    return true;
  }

  function queueBrowserDownload(bytes, name) {
    // Callers pass an owned Uint8Array copy rather than a live Emscripten heap
    // view, so the Blob remains valid if WebAssembly memory grows later.
    var stableBytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    return queueBrowserDownloadBlob(
      new Blob([stableBytes], { type: 'application/octet-stream' }), name);
  }

  function readDownloadBytes(path) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    return FS.readFile(path, { encoding: 'binary' });
  }

  function queueDownloadFromPath(path, name) {
    return queueBrowserDownload(readDownloadBytes(path), name);
  }

  Module.neoToolsBrowserFiles = {
    // Internal bridge used by the wx compatibility dialogs. It deliberately
    // shares the same ownership, limits, write-back hooks, and diagnostics as
    // requestOpenFiles() rather than creating a second browser-I/O subsystem.
    importHostEntries: function(entries, options) {
      return importSelectedHostFiles(entries || [], options || {});
    },

    registerWritablePath: function(path, name, handle) {
      registerBrowserWritablePath(path, name, { handle: handle || null });
    },

    registerWritableDirectory: function(root, handle) {
      registerBrowserWritableDirectory(root, handle || null);
    },

    releaseWritablePath: function(path) {
      forgetBrowserWritablePath(path);
    },

    releaseWritableDirectory: function(root) {
      releaseBrowserWritableDirectory(root);
    },

    releasePath: function(path) {
      path = String(path || '');
      if (!path) return;
      if (legacyImportPathSessions.has(path)) releaseLegacyImportedPaths([path]);
      else releaseBrowserOutputPath(path);
    },

    releaseDirectory: function(root) {
      root = normalizeBrowserWritableDirectory(root);
      releaseLegacyImportedDirectory(root);
      releaseBrowserOutputDirectory(root);
    },

    scheduleWritablePath: function(path) {
      scheduleBrowserWritableDownload(path);
    },

    consumeWritablePath: function(path) {
      consumeScheduledBrowserDownload(path);
    },

    publishPath: function(path, name) {
      consumeScheduledBrowserDownload(path);
      return enqueueBrowserWritablePublish(path, name || '');
    },

    chooseAndImportFiles: function(options) {
      return importHostFiles(options || {});
    },

    chooseAndImportDirectory: function(options) {
      return chooseLegacyDirectory(options || {});
    },

    createWritableFile: function(name, handle) {
      return createBrowserWritableFile(name, handle || null);
    },

    createWritableDirectory: function(name, handle) {
      return createBrowserWritableDirectory(name, handle || null);
    },

    requestRetainedFiles: function(requestId, options) {
      try {
        Promise.resolve(chooseRetainedFiles(options)).then(
          function(result) { completeRetainedFileSetRequest(requestId, result, ''); },
          function(error) {
            var message = error && error.message ? error.message : String(error || 'Unknown retained-file error.');
            console.error('[NeoTools] Retained browser-file selection failed:', error);
            completeRetainedFileSetRequest(requestId, null, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown retained-file error.');
        completeRetainedFileSetRequest(requestId, null, message);
        return true;
      }
    },

    requestRetainedDirectory: function(requestId, options) {
      try {
        Promise.resolve(chooseRetainedDirectory(options)).then(
          function(result) { completeRetainedFileSetRequest(requestId, result, ''); },
          function(error) {
            var message = error && error.message ? error.message : String(error || 'Unknown retained-directory error.');
            console.error('[NeoTools] Retained browser-directory selection failed:', error);
            completeRetainedFileSetRequest(requestId, null, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown retained-directory error.');
        completeRetainedFileSetRequest(requestId, null, message);
        return true;
      }
    },

    releaseRetainedFileSet: function(sessionId) {
      var session = retainedFileSets.get(Number(sessionId));
      if (!session) return;
      session.active = false;
      for (var entry of session.files.values()) entry.active = false;
      session.files.clear();
      retainedFileSets.delete(Number(sessionId));
    },

    retainOnlyRetainedFiles: function(sessionId, payload) {
      var session = getRetainedSession(sessionId);
      var allowed = Object.create(null);
      var values = String(payload || '').split(',');
      for (var index = 0; index < values.length; ++index) {
        if (!values[index]) continue;
        var fileId = Number(values[index]);
        if (!Number.isInteger(fileId) || fileId <= 0) {
          throw new Error('Invalid retained browser-file ID.');
        }
        allowed[fileId] = true;
      }
      for (var key of Array.from(session.files.keys())) {
        if (!allowed[key]) {
          var removed = session.files.get(key);
          if (removed) removed.active = false;
          session.files.delete(key);
        }
      }
      if (!session.files.size) {
        session.active = false;
        retainedFileSets.delete(Number(sessionId));
      }
    },

    readRetainedFileRange: function(sessionId, fileId, offset, length, destination) {
      return readRetainedFileRange(sessionId, fileId, offset, length, destination);
    },

    requestExportRetainedFiles: function(requestId, mode, defaultName, payload) {
      try {
        Promise.resolve(exportRetainedEntries(mode, defaultName, payload)).then(
          function(result) { completeRetainedExportRequest(requestId, result, ''); },
          function(error) {
            if (error && error.name === 'AbortError') {
              completeRetainedExportRequest(requestId, { disposition: 0 }, '');
              return;
            }
            var message = error && error.message ? error.message : String(error || 'Unknown retained export error.');
            console.error('[NeoTools] Retained-file export failed:', error);
            completeRetainedExportRequest(requestId, {}, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown retained export error.');
        completeRetainedExportRequest(requestId, {}, message);
        return true;
      }
    },

    retainedDirectoryWriteSupported: function() {
      return retainedDirectoryWriteSupported();
    },

    requestOpenFiles: function(requestId, options) {
      try {
        // Calling the async function starts chooseHostFiles() immediately,
        // while this invocation still has the user's click activation. Only the
        // completion is deferred.
        var importPromise = importHostFiles(options);
        Promise.resolve(importPromise).then(
          function(paths) { completeOpenFilesRequest(requestId, paths, ''); },
          function(error) {
            var message = error && error.message ? error.message : String(error || 'Unknown browser file error.');
            console.error('[NeoTools] Browser file picker failed:', error);
            completeOpenFilesRequest(requestId, [], message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser file error.');
        console.error('[NeoTools] Browser file picker request failed:', error);
        completeOpenFilesRequest(requestId, [], message);
        return true;
      }
    },

    chooseOpenFiles: function(options) {
      return importHostFiles(options);
    },

    releaseImportedFiles: function(paths) {
      releaseLegacyImportedPaths(paths || []);
    },

    resourceDiagnostics: function() {
      return {
        preparedDownloadCount: activeDownloadEntries.length,
        preparedDownloadBytes: activeDownloadBytes,
        legacyImportSessionCount: legacyImportSessions.size,
        legacyImportBytes: legacyImportBytes,
        browserWritableFileCount: browserWritableFiles.size,
        browserWritableDirectoryCount: browserWritableDirectories.size,
        browserWritablePublishCount: browserWritablePublishes.size,
        retainedFileSetCount: retainedFileSets.size,
        limits: {
          maxPreparedDownloads: MAX_PREPARED_DOWNLOADS,
          maxPreparedDownloadBytes: MAX_PREPARED_DOWNLOAD_BYTES,
          maxPreparedDownloadAgeMs: MAX_PREPARED_DOWNLOAD_AGE_MS,
          maxFallbackZipBytes: MAX_FALLBACK_ZIP_BYTES,
          maxLegacyImportSessions: MAX_LEGACY_IMPORT_SESSIONS,
          maxLegacyImportFiles: MAX_LEGACY_IMPORT_FILES,
          maxLegacyImportFileBytes: MAX_LEGACY_IMPORT_FILE_BYTES,
          maxLegacyImportSessionBytes: MAX_LEGACY_IMPORT_SESSION_BYTES,
          maxLegacyImportTotalBytes: MAX_LEGACY_IMPORT_TOTAL_BYTES,
          maxLegacyImportAgeMs: MAX_LEGACY_IMPORT_AGE_MS,
          maxBrowserWritableOutputs: MAX_BROWSER_WRITABLE_OUTPUTS,
          maxBrowserWritableDirectories: MAX_BROWSER_WRITABLE_DIRECTORIES,
          maxBrowserWritebackBytes: MAX_BROWSER_WRITEBACK_BYTES,
          maxWritableTombstoneAgeMs: MAX_WRITABLE_TOMBSTONE_AGE_MS,
          maxImportPathDepth: MAX_IMPORT_PATH_DEPTH
        }
      };
    },

    clearPreparedDownloads: function() {
      while (activeDownloadEntries.length) removeDownloadEntry(activeDownloadEntries[0]);
    },

    chooseSaveFile: function(options) {
      if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
      options = options || {};
      var fallback = appendDefaultBrowserExtension(
        options.defaultFile, options.defaultExtension);
      var chosen = window.prompt(options.title || 'Download file as', fallback);
      if (chosen === null) return null;
      chosen = appendDefaultBrowserExtension(chosen || fallback, options.defaultExtension);
      return createBrowserWritableFile(chosen, null);
    },

    requestDownloadFile: function(requestId, path, downloadName) {
      consumeScheduledBrowserDownload(path);
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      try {
        enqueueBrowserWritablePublish(path, name).then(
          function(disposition) { completeDownloadRequest(requestId, disposition, ''); },
          function(error) {
            var message = error && error.message ? error.message : String(error || 'Unknown browser save error.');
            console.error('[NeoTools] Browser save request failed:', error);
            completeDownloadRequest(requestId, 0, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser save error.');
        console.error('[NeoTools] Browser save request failed:', error);
        completeDownloadRequest(requestId, 0, message);
        return true;
      }
    },

    prepareDownloadBytes: function(bytes, downloadName) {
      var name = safeBrowserFileName(downloadName, 'download.bin');
      return queueBrowserDownload(bytes, name);
    },

    downloadFile: function(path, downloadName) {
      consumeScheduledBrowserDownload(path);
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      try {
        enqueueBrowserWritablePublish(path, name).catch(function(error) {
          console.error('[NeoTools] Browser save failed:', error);
        });
        return true;
      } catch (error) {
        console.error('[NeoTools] Browser save failed:', error);
        return false;
      }
    },

    packageDirectorySupported: function() {
      return packageDirectorySupported();
    },

    requestPackageDirectory: function(requestId) {
      try {
        var selectionPromise = choosePackageDirectory();
        Promise.resolve(selectionPromise).then(
          function(result) {
            completePackageDirectoryRequest(
              requestId, result.sessionId, result.displayName, result.iniPaths, '');
          },
          function(error) {
            if (error && error.name === 'AbortError') {
              completePackageDirectoryRequest(requestId, 0, '', [], '');
              return;
            }
            var message = error && error.message
              ? error.message
              : String(error || 'Unknown installer-folder error.');
            console.error('[NeoTools] Installer-folder selection failed:', error);
            completePackageDirectoryRequest(requestId, 0, '', [], message);
          });
        return true;
      } catch (error) {
        var message = error && error.message
          ? error.message
          : String(error || 'Unknown installer-folder error.');
        console.error('[NeoTools] Installer-folder request failed:', error);
        completePackageDirectoryRequest(requestId, 0, '', [], message);
        return true;
      }
    },

    requestPackageWorkspace: function(requestId, sessionId, relativeIniPath, relativeFiles) {
      try {
        var workspacePromise = preparePackageWorkspace(
          sessionId, relativeIniPath, relativeFiles);
        Promise.resolve(workspacePromise).then(
          function(result) { completePackageWorkspaceRequest(requestId, result, ''); },
          function(error) {
            var message = error && error.message
              ? error.message
              : String(error || 'Unknown installer-package preflight error.');
            console.error('[NeoTools] Installer-package preflight failed:', error);
            completePackageWorkspaceRequest(requestId, { sessionId: sessionId }, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message
          ? error.message
          : String(error || 'Unknown installer-package preflight error.');
        console.error('[NeoTools] Installer-package preflight request failed:', error);
        completePackageWorkspaceRequest(requestId, { sessionId: sessionId }, message);
        return true;
      }
    },

    requestCommitPackageWorkspace: function(
        requestId, sessionId, workspaceRoot, relativeIniPath, relativeFiles) {
      try {
        var commitPromise = commitPackageWorkspace(
          sessionId, workspaceRoot, relativeIniPath, relativeFiles);
        Promise.resolve(commitPromise).then(
          function(result) { completePackageCommitRequest(requestId, result, ''); },
          function(error) {
            var message = error && error.message
              ? error.message
              : String(error || 'Unknown installer-package commit error.');
            console.error('[NeoTools] Installer-package commit failed:', error);
            completePackageCommitRequest(requestId, {}, message);
          });
        return true;
      } catch (error) {
        var message = error && error.message
          ? error.message
          : String(error || 'Unknown installer-package commit error.');
        console.error('[NeoTools] Installer-package commit request failed:', error);
        completePackageCommitRequest(requestId, {}, message);
        return true;
      }
    }
  };

  Module.preRun = Module.preRun || [];
  Module.preRun.push(function() {
    if (typeof FS === 'undefined' || typeof IDBFS === 'undefined') return;
    try {
      if (typeof ENV !== 'undefined') ENV.HOME = '/home/web_user';
      FS.mkdirTree(persistentRoot);
      try { FS.mount(IDBFS, {}, persistentRoot); } catch (mountError) {
        // EBUSY means the mount already exists after a soft reload.
        if (!mountError || mountError.errno !== 10) console.warn('[NeoTools] IDBFS mount:', mountError);
      }
      addRunDependency('neotools-idbfs');
      FS.syncfs(true, function(error) {
        if (error) console.warn('[NeoTools] Unable to restore browser settings:', error);
        removeRunDependency('neotools-idbfs');
      });
    } catch (error) {
      console.warn('[NeoTools] Persistent settings are unavailable:', error);
    }
  });

  function flushSettings() {
    if (syncing || typeof FS === 'undefined' || typeof IDBFS === 'undefined') return;
    syncing = true;
    FS.syncfs(false, function(error) {
      syncing = false;
      if (error) console.warn('[NeoTools] Unable to persist browser settings:', error);
    });
  }

  window.neoToolsFlushSettings = flushSettings;
  window.addEventListener('pagehide', function() {
    while (activeDownloadEntries.length) {
      removeDownloadEntry(activeDownloadEntries[0]);
    }
    for (var importSession of Array.from(legacyImportSessions.values())) {
      removeLegacyImportSession(importSession);
    }
    for (var writableTimer of browserWritableTimers.values()) window.clearTimeout(writableTimer);
    browserWritableTimers.clear();
    browserWritablePublishes.clear();
    for (var tombstone of browserWritableTombstones.values()) {
      if (tombstone.timer) window.clearTimeout(tombstone.timer);
    }
    browserWritableTombstones.clear();
    browserWritableFiles.clear();
    browserWritableDirectories.clear();
    flushSettings();
  });
  document.addEventListener('visibilitychange', function() {
    if (document.visibilityState === 'hidden') flushSettings();
  });
  window.setInterval(flushSettings, 30000);
})();
