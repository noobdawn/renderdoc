#!/bin/bash

FILENAME="$1"

if [ $# -ne 1 ]; then
	echo "Usage: $0 FILENAME";
	exit 1;
fi

if [ ! -d "${REPO_ROOT}"/dist/bin ]; then
	echo "Expected 'dist' folder in noobdawn. When building, set CMAKE_INSTALL_PREFIX to dist and make install";
	exit 1;
fi

FILENAME="$(echo $FILENAME | tr A-Z a-z)";

rm -rf "${REPO_ROOT}"/package
mkdir "${REPO_ROOT}"/package
pushd "${REPO_ROOT}"/package

mkdir -p ${FILENAME}/

# copy in the target directory tree, cmake will have done most of our packaging for us
cp -R "${REPO_ROOT}"/dist/* "./${FILENAME}/"

# copy readme and license to the package root
cp "./${FILENAME}"/share/doc/noobdawn/{LICENSE.md,README} "./${FILENAME}"

# copy in html documentation
if [ -d "${REPO_ROOT}"/Documentation/html ]; then
	cp -R "${REPO_ROOT}"/Documentation/html "./${FILENAME}/share/doc/noobdawn/html"
else
	echo "WARNING: Documentation not built! run 'make html' in docs folder";

	if [[ "$STRICT" == "yes" ]]; then
		echo "Strict mode: Failed to locate documentation.";

		popd
		rm -rf "${REPO_ROOT}"/package
		exit 1;
	fi
fi

# copy in plugins
if [ -d "${REPO_ROOT}"/plugins-linux64 ]; then
	cp -R "${REPO_ROOT}"/plugins-linux64 "./${FILENAME}/share/noobdawn/plugins"
	chmod +x -R "./${FILENAME}/share/noobdawn/plugins"/*
else
	echo "WARNING: Plugins not present. Download and extract https://noobdawn.org/plugins.tgz in root folder";

	if [[ "$STRICT" == "yes" ]]; then
		echo "Strict mode: Failed to locate plugins.";

		popd
		rm -rf "${REPO_ROOT}"/package
		exit 1;
	fi
fi

# copy in all of the android files.
mkdir -p "./${FILENAME}/share/noobdawn/plugins/android/"

if ls "${REPO_ROOT}"/build-android*/bin/*.apk; then
	cp "${REPO_ROOT}"/build-android*/bin/*.apk "./${FILENAME}/share/noobdawn/plugins/android/"
else
	echo "WARNING: Android build not present. Build arm32 and arm64 apks in build-android-arm{32,64} folders";

	if [[ "$STRICT" == "yes" ]]; then
		echo "Strict mode: Failed to locate Android.";

		popd
		rm -rf "${REPO_ROOT}"/package
		exit 1;
	fi
fi

# compress the folder and GPG sign it
tar -zcf ${FILENAME}.tar.gz "./${FILENAME}/"* --xform 's#^./##g'
gpg -o ${FILENAME}.tar.gz.sig --detach-sign --armor ${FILENAME}.tar.gz

if [[ "$STRICT" == "yes" ]] && [ ! -f ${FILENAME}.tar.gz.sig ]; then
	echo "Strict mode: Failed to sign tarball.";

	popd
	rm -rf "${REPO_ROOT}"/package
	exit 1;
fi

rm -rf "./${FILENAME}"

popd # $REPO_ROOT/package
