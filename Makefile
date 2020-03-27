VERSION=$(shell cat emscripten-version.txt | sed s/\"//g)
GIT_HASH=$(shell git rev-parse HEAD)
DISTDIR=../emscripten-$(VERSION)
EXCLUDES=tests/third_party site __pycache__ node_modules docs Makefile
DISTFILE=emscripten-$(VERSION).tar.bz2
EXCLUDE_PATTERN=--exclude='*.pyc' --exclude='*/__pycache__'

dist: $(DISTFILE)

install:
	@rm -rf $(DISTDIR)
	mkdir $(DISTDIR)
	cp -ar * $(DISTDIR)
	echo "$(GIT_HASH)" > $(DISTDIR)/emscripten-revision.txt
	for exclude in $(EXCLUDES); do rm -rf $(DISTDIR)/$$exclude; done

# Create an distributable archive of emscripten suitable for use
# by end users.  This archive excludes parts of the codebase that
# are you only used by emscripten developers.
$(DISTFILE): install
	tar cf $@ $(EXCLUDE_PATTERN) -C `dirname $(DISTDIR)` `basename $(DISTDIR)`

.PHONY: dist install
