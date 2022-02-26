// Copyright 2018 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main() {
  const char *filename = "test.dat";
  const char *dirname = "test";

  // Create a file
  FILE* f = fopen(filename, "w+");
  if (f == NULL) {
    return 1;
  }
  // Check it exists
  if (access(filename, F_OK) != 0) {
    return 1;
  }
  // Delete the file
  if (unlinkat(AT_FDCWD, filename, 0)) {
    return 1;
  }
  // Check that it doesn't exist
  if (access(filename, F_OK) != -1) {
    return 1;
  }
  // Check that we can still write to it
  if (fwrite("hello", 1, 5, f) != 5) {
    return 1;
  }
  // And seek in it.
  if (fseek(f, 0, SEEK_SET) != 0) {
    return 1;
  }
  // And read from it.
  char buf[6] = {0};
  if (fread(buf, 1, 5, f) != 5 || strcmp("hello", buf) != 0) {
    return 1;
  }
  if (fclose(f)) {
    return 1;
  }

  // Create a directory
  if (mkdir(dirname, 0700) != 0) {
    return 1;
  }
  // Open the directory
  DIR* d = opendir(dirname);
  if (d == NULL) {
    return 1;
  }
  // Delete the directory
  if (unlinkat(AT_FDCWD, dirname, AT_REMOVEDIR)) {
    return 1;
  }
  // Check that it doesn't exist
  if (access(dirname, F_OK) != -1) {
    return 1;
  }
  // Check that we can still read the directory, but that it is empty.
  errno = 0;
  if (readdir(d) != NULL || errno != 0) {
    return 1;
  }
  // Check that we *cannot* create a child
  if (openat(dirfd(d), filename, O_CREAT | O_WRONLY, S_IRWXU) != -1) {
    return 1;
  }

  closedir(d);

  printf("ok\n");

  return 0;
}
