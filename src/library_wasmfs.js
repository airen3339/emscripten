/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

mergeInto(LibraryManager.library, {
  $wasmFSPreloadedFiles: [],
  $wasmFSPreloadedDirs: [],
#if USE_CLOSURE_COMPILER
  // Declare variable for Closure, FS.createPreloadedFile() below calls Browser.handledByPreloadPlugin()
  $FS__postset: '/**@suppress {duplicate, undefinedVars}*/var Browser;',
#endif
  $FS__deps: [
    '$wasmFSPreloadedFiles',
    '$wasmFSPreloadedDirs',
    '$asyncLoad',
    '$PATH',
    '$allocateUTF8',
    '$allocateUTF8OnStack',
    '$withStackSave',
    '$readI53FromI64',
  ],
  $FS : {
    // TODO: Clean up the following functions - currently copied from library_fs.js directly.
    createPreloadedFile: (parent, name, url, canRead, canWrite, onload, onerror, dontCreateFile, canOwn, preFinish) => {
      // TODO: use WasmFS code to resolve and join the path here?
      var fullname = name ? parent + '/' + name : parent;
      var dep = getUniqueRunDependency('cp ' + fullname); // might have several active requests for the same fullname
      function processData(byteArray) {
        function finish(byteArray) {
          if (preFinish) preFinish();
          if (!dontCreateFile) {
            FS.createDataFile(parent, name, byteArray, canRead, canWrite, canOwn);
          }
          if (onload) onload();
          removeRunDependency(dep);
        }
#if !MINIMAL_RUNTIME
        if (Browser.handledByPreloadPlugin(byteArray, fullname, finish, () => {
          if (onerror) onerror();
          removeRunDependency(dep);
        })) {
          return;
        }
#endif
        finish(byteArray);
      }
      addRunDependency(dep);
      if (typeof url == 'string') {
        asyncLoad(url, (byteArray) => {
          processData(byteArray);
        }, onerror);
      } else {
        processData(url);
      }
    },
    getMode: (canRead, canWrite) => {
      var mode = 0;
      if (canRead) mode |= {{{ cDefs.S_IRUGO }}} | {{{ cDefs.S_IXUGO }}};
      if (canWrite) mode |= {{{ cDefs.S_IWUGO }}};
      return mode;
    },
    modeStringToFlags: (str) => {
      var flags = FS.flagModes[str];
      if (typeof flags == 'undefined') {
        throw new Error('Unknown file open mode: ' + str);
      }
      return flags;
    },
    flagModes: {
      // copied directly from library_fs.js
      // Extra quotes used here on the keys to this object otherwise jsifier will
      // erase them in the process of reading and then writing the JS library
      // code.
      '"r"': {{{ cDefs.O_RDONLY }}},
      '"r+"': {{{ cDefs.O_RDWR }}},
      '"w"': {{{ cDefs.O_TRUNC }}} | {{{ cDefs.O_CREAT }}} | {{{ cDefs.O_WRONLY }}},
      '"w+"': {{{ cDefs.O_TRUNC }}} | {{{ cDefs.O_CREAT }}} | {{{ cDefs.O_RDWR }}},
      '"a"': {{{ cDefs.O_APPEND }}} | {{{ cDefs.O_CREAT }}} | {{{ cDefs.O_WRONLY }}},
      '"a+"': {{{ cDefs.O_APPEND }}} | {{{ cDefs.O_CREAT }}} | {{{ cDefs.O_RDWR }}},
    },
    createDataFile: (parent, name, data, canRead, canWrite, canOwn) => {
      // Data files must be cached until the file system itself has been initialized.
      var mode = FS.getMode(canRead, canWrite);
      var pathName = name ? parent + '/' + name : parent;
      wasmFSPreloadedFiles.push({pathName: pathName, fileData: data, mode: mode});
    },
    createPath: (parent, path, canRead, canWrite) => {
      // Cache file path directory names.
      var parts = path.split('/').reverse();
      while (parts.length) {
        var part = parts.pop();
        if (!part) continue;
        var current = PATH.join2(parent, part);
        wasmFSPreloadedDirs.push({parentPath: parent, childName: part});
        parent = current;
      }
      return current;
    },
    readFile: (path, opts = {}) => {
      opts.encoding = opts.encoding || 'binary';
      if (opts.encoding !== 'utf8' && opts.encoding !== 'binary') {
        throw new Error('Invalid encoding type "' + opts.encoding + '"');
      }

      var pathName = allocateUTF8(path);

      // Copy the file into a JS buffer on the heap.
      var buf = __wasmfs_read_file(pathName);
      // The signed integer length resides in the first 8 bytes of the buffer.
      var length = {{{ makeGetValue('buf', '0', 'i53') }}};

      // Default return type is binary.
      // The buffer contents exist 8 bytes after the returned pointer.
      var ret = new Uint8Array(HEAPU8.subarray(buf + 8, buf + 8 + length));
      if (opts.encoding === 'utf8') {
        ret = UTF8ArrayToString(ret, 0);
      }

      _free(pathName);
      _free(buf);
      return ret;
    },
    cwd: () => {
      // TODO: Remove dependency on FS.cwd().
      // User code should not be using FS.cwd().
      // For file preloading, cwd should be '/' to begin with.
      return '/';
    },

#if FORCE_FILESYSTEM
    // Full JS API support
    mkdir: (path, mode) => {
      return withStackSave(() => {
        mode = mode !== undefined ? mode : 511 /* 0777 */;
        var buffer = allocateUTF8OnStack(path);
        return __wasmfs_mkdir({{{ to64('buffer') }}}, mode);
      });
    },
    // TODO: mkdirTree
    // TDOO: rmdir
    // TODO: open
    open: (path, flags, mode) => {
      flags = typeof flags == 'string' ? FS.modeStringToFlags(flags) : flags;
      mode = typeof mode == 'undefined' ? 438 /* 0666 */ : mode;
      return withStackSave(() => {
        var buffer = allocateUTF8OnStack(path);
        return __wasmfs_open({{{ to64('buffer') }}}, flags, mode);
      })
    },
    // TODO: create
    // TODO: close
    unlink: (path) => {
      return withStackSave(() => {
        var buffer = allocateUTF8OnStack(path);
        return __wasmfs_unlink(buffer);
      });
    },
    chdir: (path) => {
      return withStackSave(() => {
        var buffer = allocateUTF8OnStack(path);
        return __wasmfs_chdir(buffer);
      });
    },
    // TODO: read
    // TODO: write
    // TODO: allocate
    // TODO: mmap
    // TODO: msync
    // TODO: munmap
    writeFile: (path, data) => {
      return withStackSave(() => {
        var pathBuffer = allocateUTF8OnStack(path);
        if (typeof data == 'string') {
          var buf = new Uint8Array(lengthBytesUTF8(data) + 1);
          var actualNumBytes = stringToUTF8Array(data, buf, 0, buf.length);
          data = buf.slice(0, actualNumBytes);
        }
        var dataBuffer = _malloc(data.length);
#if ASSERTIONS
        assert(dataBuffer);
#endif
        for (var i = 0; i < data.length; i++) {
          {{{ makeSetValue('dataBuffer', 'i', 'data[i]', 'i8') }}};
        }
        var ret = __wasmfs_write_file(pathBuffer, dataBuffer, data.length);
        _free(dataBuffer);
        return ret;
      });
    },
    symlink: (target, linkpath) => {
      return withStackSave(() => {
        var targetBuffer = allocateUTF8OnStack(target);
        var linkpathBuffer = allocateUTF8OnStack(linkpath);
        return __wasmfs_symlink(targetBuffer, linkpathBuffer);
      });
    },
    // TODO: readlink
    // TODO: stat
    // TODO: lstat
    chmod: (path, mode) => {
      return withStackSave(() => {
        var buffer = allocateUTF8OnStack(path);
        return __wasmfs_chmod(buffer, mode);
      });
    },
    // TODO: lchmod
    // TODO: fchmod
    // TDOO: chown
    // TODO: lchown
    // TODO: fchown
    // TODO: truncate
    // TODO: ftruncate
    // TODO: utime
    findObject: (path) => {
      var result = __wasmfs_identify(path);
      if (result == {{{ cDefs.ENOENT }}}) {
        return null;
      }
      return {
        isFolder: result == {{{ cDefs.EISDIR }}},
        isDevice: false, // TODO: wasmfs support for devices
      };
    },
    readdir: (path) => {
      return withStackSave(() => {
        var pathBuffer = allocateUTF8OnStack(path);
        var entries = [];
        var state = __wasmfs_readdir_start(pathBuffer);
        if (!state) {
          // TODO: The old FS threw an ErrnoError here.
          throw new Error("No such directory");
        }
        var entry;
        while (entry = __wasmfs_readdir_get(state)) {
          entries.push(UTF8ToString(entry));
        }
        __wasmfs_readdir_finish(state);
        return entries;
      });
    }
    // TODO: mount
    // TODO: unmount
    // TODO: lookup
    // TODO: mknod
    // TODO: mkdev
    // TODO: rename
    // TODO: syncfs
    // TODO: llseek
    // TODO: ioctl

#endif
  },
  _wasmfs_get_num_preloaded_files__deps: ['$wasmFSPreloadedFiles'],
  _wasmfs_get_num_preloaded_files: function() {
    return wasmFSPreloadedFiles.length;
  },
  _wasmfs_get_num_preloaded_dirs__deps: ['$wasmFSPreloadedDirs'],
  _wasmfs_get_num_preloaded_dirs: function() {
    return wasmFSPreloadedDirs.length;
  },
  _wasmfs_get_preloaded_file_mode: function(index) {
    return wasmFSPreloadedFiles[index].mode;
  },
  _wasmfs_get_preloaded_parent_path__sig: 'vip',
  _wasmfs_get_preloaded_parent_path: function(index, parentPathBuffer) {
    var s = wasmFSPreloadedDirs[index].parentPath;
    var len = lengthBytesUTF8(s) + 1;
    stringToUTF8(s, parentPathBuffer, len);
  },
  _wasmfs_get_preloaded_child_path__sig: 'vip',
  _wasmfs_get_preloaded_child_path: function(index, childNameBuffer) {
    var s = wasmFSPreloadedDirs[index].childName;
    var len = lengthBytesUTF8(s) + 1;
    stringToUTF8(s, childNameBuffer, len);
  },
  _wasmfs_get_preloaded_path_name__sig: 'vip',
  _wasmfs_get_preloaded_path_name: function(index, fileNameBuffer) {
    var s = wasmFSPreloadedFiles[index].pathName;
    var len = lengthBytesUTF8(s) + 1;
    stringToUTF8(s, fileNameBuffer, len);
  },
  _wasmfs_get_preloaded_file_size__sig: 'pi',
  _wasmfs_get_preloaded_file_size: function(index) {
    return wasmFSPreloadedFiles[index].fileData.length;
  },
  _wasmfs_copy_preloaded_file_data__sig: 'vip',
  _wasmfs_copy_preloaded_file_data: function(index, buffer) {
    HEAPU8.set(wasmFSPreloadedFiles[index].fileData, buffer);
  }
});

DEFAULT_LIBRARY_FUNCS_TO_INCLUDE.push('$FS');
