import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const bootstrapSource = fs.readFileSync(path.join(here, '..', 'wasm', 'neo-pre.js'), 'utf8');

class MockElement {
  constructor(tagName, document) {
    this.tagName = String(tagName || '').toUpperCase();
    this.ownerDocument = document;
    this.children = [];
    this.parentNode = null;
    this.style = {};
    this.listeners = new Map();
    this.attributes = new Map();
    this.id = '';
    this.textContent = '';
    this.type = '';
    this.href = '';
    this.download = '';
    this.title = '';
    this.files = [];
  }
  appendChild(child) {
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  prepend(child) {
    child.parentNode = this;
    this.children.unshift(child);
    return child;
  }
  removeChild(child) {
    const index = this.children.indexOf(child);
    if (index >= 0) this.children.splice(index, 1);
    child.parentNode = null;
    return child;
  }
  remove() {
    if (this.parentNode) this.parentNode.removeChild(this);
  }
  setAttribute(name, value) {
    this.attributes.set(String(name), String(value));
    if (name === 'id') this.id = String(value);
  }
  addEventListener(name, callback) {
    this.listeners.set(String(name), callback);
  }
  focus() {}
  click() {}
}

function findElementById(root, id) {
  if (root.id === id) return root;
  for (const child of root.children) {
    const found = findElementById(child, id);
    if (found) return found;
  }
  return null;
}

function normalizeVirtualPath(value) {
  let result = String(value || '').replaceAll('\\', '/');
  if (!result.startsWith('/')) result = '/' + result;
  const parts = [];
  for (const part of result.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return '/' + parts.join('/');
}

function parentPath(value) {
  const normalized = normalizeVirtualPath(value);
  const index = normalized.lastIndexOf('/');
  return index <= 0 ? '/' : normalized.slice(0, index);
}

function createMockFs() {
  const files = new Map();
  const directories = new Set(['/']);

  function mkdirTree(value) {
    const normalized = normalizeVirtualPath(value);
    let current = '';
    for (const part of normalized.split('/').filter(Boolean)) {
      current += '/' + part;
      directories.add(current);
    }
  }

  function bytesOf(value) {
    if (typeof value === 'string') return new TextEncoder().encode(value);
    if (value instanceof Uint8Array) return new Uint8Array(value);
    if (ArrayBuffer.isView(value)) {
      return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
    }
    if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
    return new Uint8Array(value || []);
  }

  const api = {
    files,
    directories,
    mkdirTree,
    writeFile(value, data) {
      const filePath = normalizeVirtualPath(value);
      mkdirTree(parentPath(filePath));
      files.set(filePath, bytesOf(data));
    },
    readFile(value) {
      const filePath = normalizeVirtualPath(value);
      if (!files.has(filePath)) throw new Error(`ENOENT: ${filePath}`);
      return new Uint8Array(files.get(filePath));
    },
    stat(value) {
      const filePath = normalizeVirtualPath(value);
      if (files.has(filePath)) return { size: files.get(filePath).byteLength, mode: 0 };
      if (directories.has(filePath)) return { size: 0, mode: 0 };
      throw new Error(`ENOENT: ${filePath}`);
    },
    unlink(value) {
      const filePath = normalizeVirtualPath(value);
      if (!files.delete(filePath)) throw new Error(`ENOENT: ${filePath}`);
    },
    rmdir(value) {
      const directory = normalizeVirtualPath(value);
      for (const filePath of files.keys()) {
        if (filePath.startsWith(directory + '/')) throw new Error(`ENOTEMPTY: ${directory}`);
      }
      for (const child of directories) {
        if (child !== directory && child.startsWith(directory + '/')) {
          throw new Error(`ENOTEMPTY: ${directory}`);
        }
      }
      directories.delete(directory);
    },
    rename(oldValue, newValue) {
      const oldPath = normalizeVirtualPath(oldValue);
      const newPath = normalizeVirtualPath(newValue);
      if (!files.has(oldPath)) throw new Error(`ENOENT: ${oldPath}`);
      mkdirTree(parentPath(newPath));
      files.set(newPath, files.get(oldPath));
      files.delete(oldPath);
    },
    close() {},
    getPath(node) { return node.path; },
    mount() {},
    syncfs(_populate, callback) { callback(null); },
  };
  return api;
}

class MockFile {
  constructor(name, bytes, relativePath = '') {
    this.name = name;
    this._bytes = new Uint8Array(bytes);
    this.size = this._bytes.byteLength;
    this.webkitRelativePath = relativePath;
  }
  async arrayBuffer() {
    const copy = new Uint8Array(this._bytes);
    return copy.buffer;
  }
  slice(start, end) {
    return new Blob([this._bytes.slice(start, end)]);
  }
}

class MockFileHandle {
  constructor(name, fileBytes = []) {
    this.kind = 'file';
    this.name = name;
    this.bytes = new Uint8Array(fileBytes);
    this.writeCount = 0;
  }
  async getFile() { return new MockFile(this.name, this.bytes); }
  async createWritable() {
    const handle = this;
    let pending = new Uint8Array();
    return {
      async write(value) {
        if (value instanceof Blob) value = new Uint8Array(await value.arrayBuffer());
        if (value instanceof Uint8Array) pending = new Uint8Array(value);
        else if (ArrayBuffer.isView(value)) {
          pending = new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
        } else if (value instanceof ArrayBuffer) pending = new Uint8Array(value.slice(0));
        else pending = new Uint8Array(value || []);
      },
      async close() {
        handle.bytes = pending;
        handle.writeCount += 1;
      },
      async abort() {},
    };
  }
}

class MockDirectoryHandle {
  constructor(name) {
    this.kind = 'directory';
    this.name = name;
    this.directories = new Map();
    this.files = new Map();
    this.entriesRead = 0;
  }
  async getDirectoryHandle(name, options = {}) {
    if (!this.directories.has(name)) {
      if (!options.create) throw new Error(`Missing directory ${name}`);
      this.directories.set(name, new MockDirectoryHandle(name));
    }
    return this.directories.get(name);
  }
  async getFileHandle(name, options = {}) {
    if (!this.files.has(name)) {
      if (!options.create) throw new Error(`Missing file ${name}`);
      this.files.set(name, new MockFileHandle(name));
    }
    return this.files.get(name);
  }
  async *entries() {
    this.entriesRead += 1;
    for (const entry of this.directories) yield entry;
    for (const entry of this.files) yield entry;
  }
}

function createRuntime(limits = {}) {
  const virtualFs = createMockFs();
  const document = {
    visibilityState: 'visible',
    body: null,
    createElement(tag) { return new MockElement(tag, this); },
    getElementById(id) { return findElementById(this.body, id); },
    addEventListener() {},
  };
  document.body = new MockElement('body', document);
  const revokedUrls = [];
  let nextUrl = 0;
  const windowListeners = new Map();
  const window = {
    prompt(_title, fallback) { return fallback; },
    addEventListener(name, callback) { windowListeners.set(name, callback); },
    removeEventListener(name) { windowListeners.delete(name); },
    setTimeout(callback, milliseconds) {
      const timer = setTimeout(callback, milliseconds);
      if (milliseconds > 1000 && typeof timer.unref === 'function') timer.unref();
      return timer;
    },
    clearTimeout,
    setInterval() { return 0; },
    clearInterval() {},
  };
  const Module = { preRun: [], neoToolsBrowserLimits: limits };
  const messages = { warnings: [], errors: [] };
  const runtimeConsole = {
    log: (...args) => console.log(...args),
    warn: (...args) => messages.warnings.push(args.map(String).join(' ')),
    error: (...args) => messages.errors.push(args.map(String).join(' ')),
  };
  const context = {
    Module,
    FS: virtualFs,
    IDBFS: {},
    ENV: {},
    window,
    document,
    Blob,
    Uint8Array,
    ArrayBuffer,
    TextEncoder,
    URL: {
      createObjectURL() { nextUrl += 1; return `blob:mock-${nextUrl}`; },
      revokeObjectURL(url) { revokedUrls.push(url); },
    },
    console: runtimeConsole,
    Date,
    Map,
    Set,
    Promise,
    Number,
    String,
    Object,
    Array,
    Math,
    RegExp,
    Error,
    TypeError,
    decodeURIComponent,
    encodeURIComponent,
    setTimeout,
    clearTimeout,
    addRunDependency() {},
    removeRunDependency() {},
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext(bootstrapSource, context, { filename: 'neo-pre.js' });
  assert.ok(Module.neoToolsBrowserFiles, 'bootstrap must expose the browser-file API');
  return {
    context, Module, FS: virtualFs, window, document, revokedUrls, messages,
    api: Module.neoToolsBrowserFiles,
  };
}

const bytes = value => Array.from(value);
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function testDirectSaveToHandle() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('document.gff', [1, 2, 3]);
  const imported = await runtime.api.importHostEntries([
    { file: await handle.getFile(), handle, relativePath: 'document.gff' },
  ]);
  assert.equal(imported.paths.length, 1);
  const target = imported.paths[0];
  runtime.FS.writeFile(target, new Uint8Array([9, 8, 7]));
  runtime.FS.close({ path: target, flags: 1 });
  await pause(60);
  assert.deepEqual(bytes(handle.bytes), [9, 8, 7]);
  assert.equal(handle.writeCount, 1);
  assert.equal(runtime.api.resourceDiagnostics().preparedDownloadCount, 0);
  runtime.api.releaseImportedFiles([target]);
}

async function testFallbackSavePreparesDownload() {
  const runtime = createRuntime();
  const imported = await runtime.api.importHostEntries([
    { file: new MockFile('dialog.dlg', [1]), handle: null, relativePath: 'dialog.dlg' },
  ]);
  const target = imported.paths[0];
  runtime.FS.writeFile(target, new Uint8Array([4, 5, 6]));
  runtime.FS.close({ path: target, flags: 1 });
  await pause(60);
  const diagnostics = runtime.api.resourceDiagnostics();
  assert.equal(diagnostics.preparedDownloadCount, 1);
  assert.equal(diagnostics.preparedDownloadBytes, 3);
  runtime.api.clearPreparedDownloads();
  runtime.api.releaseImportedFiles([target]);
}


async function testHostWriteFailureFallsBackToDownload() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('readonly.qst', [1]);
  handle.createWritable = async () => { throw new Error('write denied'); };
  const imported = await runtime.api.importHostEntries([
    { file: await handle.getFile(), handle, relativePath: 'readonly.qst' },
  ]);
  const target = imported.paths[0];
  runtime.FS.writeFile(target, new Uint8Array([2, 3]));
  runtime.FS.close({ path: target, flags: 1 });
  await pause(60);
  assert.equal(runtime.api.resourceDiagnostics().preparedDownloadCount, 1);
  assert.equal(runtime.api.resourceDiagnostics().preparedDownloadBytes, 2);
  assert.ok(runtime.messages.warnings.some(message => message.includes('Host-file write failed')));
  runtime.api.clearPreparedDownloads();
  runtime.api.releaseImportedFiles([target]);
}

async function testAtomicReplacePreservesBinding() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('journal.jrl', [1]);
  const imported = await runtime.api.importHostEntries([
    { file: await handle.getFile(), handle, relativePath: 'journal.jrl' },
  ]);
  const target = imported.paths[0];
  const temporary = target + '.tmp';
  runtime.FS.writeFile(temporary, new Uint8Array([7, 7, 7, 7]));
  runtime.FS.unlink(target);
  runtime.FS.rename(temporary, target);
  await pause(60);
  assert.deepEqual(bytes(handle.bytes), [7, 7, 7, 7]);
  assert.equal(handle.writeCount, 1);
  runtime.api.releaseImportedFiles([target]);
}



async function testWritebacksAreSerialized() {
  const runtime = createRuntime();
  class DelayedHandle extends MockFileHandle {
    constructor(name, fileBytes) {
      super(name, fileBytes);
      this.openCount = 0;
    }
    async createWritable() {
      const handle = this;
      const ordinal = ++this.openCount;
      let pending = new Uint8Array();
      return {
        async write(value) {
          pending = value instanceof Uint8Array
            ? new Uint8Array(value)
            : new Uint8Array(await new Blob([value]).arrayBuffer());
        },
        async close() {
          await new Promise(resolve => setTimeout(resolve, ordinal === 1 ? 90 : 0));
          handle.bytes = pending;
          handle.writeCount += 1;
        },
        async abort() {},
      };
    }
  }

  const handle = new DelayedHandle('ordered.gff', [0]);
  const imported = await runtime.api.importHostEntries([
    { file: await handle.getFile(), handle, relativePath: 'ordered.gff' },
  ]);
  const target = imported.paths[0];
  runtime.FS.writeFile(target, new Uint8Array([1]));
  runtime.FS.close({ path: target, flags: 1 });
  await pause(35);
  runtime.FS.writeFile(target, new Uint8Array([2]));
  runtime.FS.close({ path: target, flags: 1 });
  await pause(180);
  assert.deepEqual(bytes(handle.bytes), [2], 'the newest save must win');
  assert.equal(handle.writeCount, 2);
  assert.equal(runtime.api.resourceDiagnostics().browserWritablePublishCount, 0);
  runtime.api.releaseImportedFiles([target]);
}

async function testPartialImportFailureCleansMemfs() {
  const runtime = createRuntime();
  const broken = {
    name: 'broken.gff',
    size: 1,
    async arrayBuffer() { throw new Error('synthetic read failure'); },
  };
  await assert.rejects(runtime.api.importHostEntries([
    { file: new MockFile('good.gff', [1]), relativePath: 'nested/good.gff' },
    { file: broken, relativePath: 'nested/deeper/broken.gff' },
  ], { preserveRelativePaths: true, rootName: 'module' }), /synthetic read failure/);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.equal(runtime.FS.files.size, 0);
  assert.ok(!Array.from(runtime.FS.directories).some(
    directory => directory.startsWith('/tmp/neotools-import')),
  'failed imports must remove all session-specific directories');
}

async function testReleaseFlushesPendingSave() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('pending.ssf', [1]);
  const imported = await runtime.api.importHostEntries([
    { file: await handle.getFile(), handle, relativePath: 'pending.ssf' },
  ]);
  const target = imported.paths[0];
  runtime.FS.writeFile(target, new Uint8Array([8, 6, 7, 5]));
  runtime.FS.close({ path: target, flags: 1 });
  runtime.api.releaseImportedFiles([target]);
  await pause(80);
  assert.deepEqual(bytes(handle.bytes), [8, 6, 7, 5]);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.throws(() => runtime.FS.readFile(target), /ENOENT/);
}

async function testCapacityRejectsWithoutEviction() {
  const runtime = createRuntime({ maxLegacyImportSessions: 1 });
  const first = await runtime.api.importHostEntries([
    { file: new MockFile('first.tlk', [1, 2]), relativePath: 'first.tlk' },
  ]);
  await assert.rejects(
    runtime.api.importHostEntries([
      { file: new MockFile('second.tlk', [3, 4]), relativePath: 'second.tlk' },
    ]),
    /Close or release an imported document/,
  );
  assert.deepEqual(bytes(runtime.FS.readFile(first.paths[0])), [1, 2]);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 1);
  runtime.api.releaseImportedFiles(first.paths);
}

async function testReleaseDeletesCompleteSession() {
  const runtime = createRuntime();
  const result = await runtime.api.importHostEntries([
    { file: new MockFile('a.2da', [1]), relativePath: 'a.2da' },
    { file: new MockFile('b.2da', [2]), relativePath: 'b.2da' },
  ]);
  runtime.api.releasePath(result.paths[0]);
  for (const importedPath of result.paths) {
    assert.throws(() => runtime.FS.readFile(importedPath), /ENOENT/);
  }
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
}

async function testWritableDirectory() {
  const runtime = createRuntime();
  const hostDirectory = new MockDirectoryHandle('exports');
  const root = runtime.api.createWritableDirectory('exports', hostDirectory);
  const virtualFile = root + '/nested/result.tga';
  runtime.FS.writeFile(virtualFile, new Uint8Array([10, 20, 30]));
  runtime.FS.close({ path: virtualFile, flags: 1 });
  await pause(60);
  const nested = hostDirectory.directories.get('nested');
  assert.ok(nested);
  const output = nested.files.get('result.tga');
  assert.ok(output);
  assert.deepEqual(bytes(output.bytes), [10, 20, 30]);
  runtime.api.releaseDirectory(root);
}

async function testOutputReleaseFlushesPendingSave() {
  const runtime = createRuntime();
  const target = runtime.api.createWritableFile('pending-output.bin', null);
  runtime.FS.writeFile(target, new Uint8Array([4, 2]));
  runtime.FS.close({ path: target, flags: 1 });
  runtime.api.releasePath(target);
  await pause(80);
  assert.equal(runtime.api.resourceDiagnostics().preparedDownloadCount, 1);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableFileCount, 0);
  runtime.api.clearPreparedDownloads();
}

async function testOutputDirectoryReleaseFlushesPendingSave() {
  const runtime = createRuntime();
  const hostDirectory = new MockDirectoryHandle('release-output');
  const root = runtime.api.createWritableDirectory('release-output', hostDirectory);
  const target = root + '/nested/file.bin';
  runtime.FS.writeFile(target, new Uint8Array([3, 1, 4]));
  runtime.FS.close({ path: target, flags: 1 });
  runtime.api.releaseDirectory(root);
  await pause(80);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableDirectoryCount, 0);
  const nested = hostDirectory.directories.get('nested');
  assert.ok(nested);
  assert.deepEqual(bytes(nested.files.get('file.bin').bytes), [3, 1, 4]);
}

async function testOutputDirectoryDoesNotImportExistingFiles() {
  const runtime = createRuntime();
  const directory = new MockDirectoryHandle('destination');
  directory.files.set('existing.bin', new MockFileHandle('existing.bin', [1, 2, 3]));
  runtime.window.showDirectoryPicker = async () => directory;
  const root = await runtime.api.chooseAndImportDirectory({ allowCreate: true });
  assert.match(root, /destination$/);
  assert.equal(directory.entriesRead, 0, 'output selection must not enumerate host contents');
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  runtime.api.releaseDirectory(root);
}


async function testOrphanedCompletionReleasesImport() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('orphan.gff', [5, 4, 3]);
  runtime.window.showOpenFilePicker = async () => [handle];
  assert.equal(runtime.api.requestOpenFiles(77, { accept: '.gff', multiple: false }), true);
  await pause(80);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0,
    'failed C++ completion delivery must release the imported session');
  assert.ok(runtime.messages.errors.some(message => message.includes('ccall is unavailable')));
}


async function testEmptyReadDirectoryIsScoped() {
  const runtime = createRuntime();
  const directory = new MockDirectoryHandle('empty-module');
  runtime.window.showDirectoryPicker = async () => directory;
  const root = await runtime.api.chooseAndImportDirectory({ allowCreate: false });
  assert.match(root, /empty-module$/);
  assert.notEqual(root, '/');
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableDirectoryCount, 1);
  runtime.api.releaseDirectory(root);
}

async function testReadDirectoryRelease() {
  const runtime = createRuntime();
  const directory = new MockDirectoryHandle('module');
  directory.files.set('one.gff', new MockFileHandle('one.gff', [1]));
  const nested = new MockDirectoryHandle('nested');
  nested.files.set('two.utc', new MockFileHandle('two.utc', [2]));
  directory.directories.set('nested', nested);
  runtime.window.showDirectoryPicker = async () => directory;
  const root = await runtime.api.chooseAndImportDirectory({ allowCreate: false });
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 1);
  assert.ok(Array.from(runtime.FS.files.keys()).some(filePath => filePath.endsWith('/one.gff')));
  assert.ok(Array.from(runtime.FS.files.keys()).some(filePath => filePath.endsWith('/nested/two.utc')));
  runtime.api.releaseDirectory(root);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableDirectoryCount, 0);
}


async function testImportValidationLimits() {
  const runtime = createRuntime({ maxLegacyImportFileBytes: 2, maxImportPathDepth: 2 });
  await assert.rejects(runtime.api.importHostEntries([
    { file: new MockFile('large.bin', [1, 2, 3]), relativePath: 'large.bin' },
  ]), /per-file import limit/);
  await assert.rejects(runtime.api.importHostEntries([
    { file: new MockFile('escape.bin', [1]), relativePath: '../escape.bin' },
  ], { preserveRelativePaths: true }), /parent components/);

  const root = new MockDirectoryHandle('deep');
  const one = new MockDirectoryHandle('one');
  const two = new MockDirectoryHandle('two');
  const three = new MockDirectoryHandle('three');
  three.files.set('payload.bin', new MockFileHandle('payload.bin', [1]));
  two.directories.set('three', three);
  one.directories.set('two', two);
  root.directories.set('one', one);
  runtime.window.showDirectoryPicker = async () => root;
  await assert.rejects(runtime.api.chooseAndImportDirectory({ allowCreate: false }),
    /nesting-depth limit/);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.equal(runtime.FS.files.size, 0);
}



async function testWritableRegistrationRollback() {
  const runtime = createRuntime({ maxBrowserWritableDirectories: 1 });
  const first = runtime.api.createWritableFile('first.bin', null);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableFileCount, 1);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableDirectoryCount, 1);
  assert.throws(() => runtime.api.createWritableFile('second.bin', null),
    /directories exceed the configured session limit/);
  assert.equal(runtime.api.resourceDiagnostics().browserWritableFileCount, 1,
    'failed output registration must roll back its file binding');
  assert.equal(runtime.api.resourceDiagnostics().browserWritableDirectoryCount, 1);
  runtime.api.releasePath(first);
  runtime.api.releaseDirectory(parentPath(first));
}

async function testWritebackByteLimit() {
  const runtime = createRuntime({ maxBrowserWritebackBytes: 2 });
  const path = runtime.api.createWritableFile('too-large.bin', null);
  runtime.FS.writeFile(path, new Uint8Array([1, 2, 3]));
  await assert.rejects(runtime.api.publishPath(path, 'too-large.bin'),
    /write-back byte limit/);
  assert.equal(runtime.api.resourceDiagnostics().preparedDownloadCount, 0);
  runtime.api.releasePath(path);
}

async function testRetainedPickerUsesHandleRecords() {
  const runtime = createRuntime();
  const handle = new MockFileHandle('archive.bif', [66, 73, 70, 70]);
  runtime.window.showOpenFilePicker = async () => [handle];
  let completion = null;
  runtime.Module.ccall = function(name, _returnType, _argumentTypes, argumentsList) {
    completion = { name, arguments: argumentsList };
    return Promise.resolve();
  };
  assert.equal(runtime.api.requestRetainedFiles(
    91, { title: 'Open BIF', accept: '.bif', multiple: false }), true);
  await pause(60);
  assert.ok(completion, 'retained picker must invoke the completion callback');
  assert.equal(completion.name, 'neo_browser_retained_file_set_completed');
  assert.equal(completion.arguments[0], 91);
  const sessionId = completion.arguments[1];
  assert.ok(sessionId > 0);
  assert.match(completion.arguments[3], /archive\.bif/);
  assert.equal(completion.arguments[4], '');
  runtime.api.releaseRetainedFileSet(sessionId);
  assert.equal(runtime.api.resourceDiagnostics().retainedFileSetCount, 0);
}

async function testOptionalLegacyImportExpiry() {
  const runtime = createRuntime({ maxLegacyImportAgeMs: 25 });
  const imported = await runtime.api.importHostEntries([
    { file: new MockFile('temporary.bin', [1]), relativePath: 'temporary.bin' },
  ]);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 1);
  await pause(70);
  assert.equal(runtime.api.resourceDiagnostics().legacyImportSessionCount, 0);
  assert.throws(() => runtime.FS.readFile(imported.paths[0]), /ENOENT/);
}

async function testPreparedDownloadEviction() {
  const runtime = createRuntime({ maxPreparedDownloads: 2, maxPreparedDownloadBytes: 64 });
  runtime.api.prepareDownloadBytes(new Uint8Array([1]), 'one.bin');
  runtime.api.prepareDownloadBytes(new Uint8Array([2]), 'two.bin');
  runtime.api.prepareDownloadBytes(new Uint8Array([3]), 'three.bin');
  const diagnostics = runtime.api.resourceDiagnostics();
  assert.equal(diagnostics.preparedDownloadCount, 2);
  assert.equal(diagnostics.preparedDownloadBytes, 2);
  assert.equal(runtime.revokedUrls.length, 1);
  runtime.api.clearPreparedDownloads();
}

await testDirectSaveToHandle();
await testFallbackSavePreparesDownload();
await testHostWriteFailureFallsBackToDownload();
await testAtomicReplacePreservesBinding();
await testWritebacksAreSerialized();
await testPartialImportFailureCleansMemfs();
await testReleaseFlushesPendingSave();
await testCapacityRejectsWithoutEviction();
await testReleaseDeletesCompleteSession();
await testWritableDirectory();
await testOutputReleaseFlushesPendingSave();
await testOutputDirectoryReleaseFlushesPendingSave();
await testOutputDirectoryDoesNotImportExistingFiles();
await testOrphanedCompletionReleasesImport();
await testEmptyReadDirectoryIsScoped();
await testReadDirectoryRelease();
await testImportValidationLimits();
await testWritableRegistrationRollback();
await testWritebackByteLimit();
await testRetainedPickerUsesHandleRecords();
await testOptionalLegacyImportExpiry();
await testPreparedDownloadEviction();

console.log('NeoShared browser-file bridge regression tests: PASS');
