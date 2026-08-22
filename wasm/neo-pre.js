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

  function safeBrowserFileName(name, fallback) {
    var value = String(name || '').replace(/[\\/\u0000-\u001f\u007f]/g, '_').trim();
    if (!value || value === '.' || value === '..') value = fallback || 'download.bin';
    return value;
  }

  function uniqueBrowserDirectory(kind) {
    browserFileSequence += 1;
    var directory = '/tmp/neotools-' + kind + '/' +
      Date.now().toString(36) + '-' + browserFileSequence.toString(36);
    FS.mkdirTree(directory);
    return directory;
  }

  function chooseHostFiles(options) {
    options = options || {};
    return new Promise(function(resolve) {
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

      // Older browsers don't emit the input cancel event. When focus returns
      // after the native picker closes, allow the change event to arrive first,
      // then treat an empty selection as cancellation.
      window.addEventListener('focus', onWindowFocus);

      document.body.appendChild(input);
      try {
        input.click();
      } catch (error) {
        console.error('[NeoTools] Unable to open the browser file picker:', error);
        finish([]);
      }
    });
  }

  async function importHostFiles(options) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    var files = await chooseHostFiles(options);
    if (!files.length) return [];

    var directory = uniqueBrowserDirectory('import');
    var used = Object.create(null);
    var paths = [];
    for (var index = 0; index < files.length; ++index) {
      var file = files[index];
      var originalName = safeBrowserFileName(file.name, 'import.bin');
      var candidate = originalName;
      var suffix = 2;
      while (used[candidate]) {
        var dot = originalName.lastIndexOf('.');
        var stem = dot > 0 ? originalName.substring(0, dot) : originalName;
        var extension = dot > 0 ? originalName.substring(dot) : '';
        candidate = stem + '-' + suffix + extension;
        suffix += 1;
      }
      used[candidate] = true;

      var bytes = new Uint8Array(await file.arrayBuffer());
      var path = directory + '/' + candidate;
      FS.writeFile(path, bytes);
      paths.push(path);
    }
    return paths;
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

  async function resolvePackageFile(rootHandle, relativePath, createParents) {
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
        directory = await directory.getDirectoryHandle(components[index], { create: true });
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
    FS.writeFile(fullPath, bytes);
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

  async function removeCreatedPackageFiles(created) {
    for (var index = created.length - 1; index >= 0; --index) {
      try {
        await created[index].parentHandle.removeEntry(created[index].name);
      } catch (error) {
        console.warn('[NeoTools] Unable to roll back package file ' + created[index].name + ':', error);
      }
    }
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
    var created = [];
    try {
      for (var planIndex = 0; planIndex < plans.length; ++planIndex) {
        var plan = plans[planIndex];
        if (plan.current.exists && bytesEqual(plan.current.bytes, plan.generated)) {
          filesReused += 1;
          continue;
        }
        var writeResult = await writePackageHostFile(
          session.handle, plan.relativePath, plan.generated);
        if (writeResult.created) created.push(writeResult);
        filesWritten += 1;
        if (plan.isIni) iniChanged = true;
      }
    } catch (error) {
      await removeCreatedPackageFiles(created);
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

  function removeDownloadItem(item, url) {
    if (item && item.parentNode) item.parentNode.removeChild(item);
    for (var index = activeDownloadEntries.length - 1; index >= 0; --index) {
      if (activeDownloadEntries[index].item === item || activeDownloadEntries[index].url === url) {
        activeDownloadEntries.splice(index, 1);
      }
    }
    try { URL.revokeObjectURL(url); } catch (_) {}
  }

  function queueBrowserDownload(bytes, name) {
    // Callers pass an owned Uint8Array copy rather than a live Emscripten heap
    // view, so the Blob remains valid if WebAssembly memory grows later.
    var stableBytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    var blob = new Blob([stableBytes], { type: 'application/octet-stream' });
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
      if (!activeDownloadEntries.length && host.parentNode) host.parentNode.removeChild(host);
    });

    content.appendChild(message);
    content.appendChild(anchor);
    item.appendChild(content);
    item.appendChild(close);
    host.prepend(item);
    activeDownloadEntries.push({ item: item, url: url, anchor: anchor });
    while (activeDownloadEntries.length > 6) {
      var oldest = activeDownloadEntries[0];
      removeDownloadItem(oldest.item, oldest.url);
    }

    // Do not synthesize a click and do not invoke a native save picker here.
    // The next click is a normal browser DOM activation on this visible link,
    // outside wxWidgets-WASM's synchronous event-dispatch interlock.
    try { anchor.focus({ preventScroll: true }); } catch (_) { try { anchor.focus(); } catch (_) {} }
    return true;
  }

  function readDownloadBytes(path) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    var bytes = FS.readFile(path, { encoding: 'binary' });
    var copy = new Uint8Array(bytes.length);
    copy.set(bytes);
    return copy;
  }

  function queueDownloadFromPath(path, name) {
    return queueBrowserDownload(readDownloadBytes(path), name);
  }

  Module.neoToolsBrowserFiles = {
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

    chooseSaveFile: function(options) {
      if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
      options = options || {};
      var fallback = safeBrowserFileName(options.defaultFile, 'download.bin');
      var chosen = window.prompt(options.title || 'Download file as', fallback);
      if (chosen === null) return null;
      chosen = safeBrowserFileName(chosen, fallback);
      return uniqueBrowserDirectory('export') + '/' + chosen;
    },

    requestDownloadFile: function(requestId, path, downloadName) {
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      try {
        queueDownloadFromPath(path, name);
        completeDownloadRequest(requestId, 2, ''); // Ready
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser download error.');
        console.error('[NeoTools] Browser download request failed:', error);
        completeDownloadRequest(requestId, 0, message);
        return true;
      }
    },

    prepareDownloadBytes: function(bytes, downloadName) {
      var name = safeBrowserFileName(downloadName, 'download.bin');
      return queueBrowserDownload(bytes, name);
    },

    downloadFile: function(path, downloadName) {
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      return queueDownloadFromPath(path, name);
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
      var entry = activeDownloadEntries.pop();
      try { URL.revokeObjectURL(entry.url); } catch (_) {}
    }
    flushSettings();
  });
  document.addEventListener('visibilitychange', function() {
    if (document.visibilityState === 'hidden') flushSettings();
  });
  window.setInterval(flushSettings, 30000);
})();
