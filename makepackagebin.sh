#!/bin/bash
echo "Script to invoke Jam and then package the binary release."

# Must use this rather than "jam -q" to ensure builtin libraries are used,
# and build environment suites a realse.

PRODUCT=Argyll

# Set the environment string VERSION from the #define, ie 1.0.0
VERSION=`grep ARGYLL_VERSION_STR h/aconfig.h | head -1 | sed 's/# define ARGYLL_VERSION_STR //' | sed 's/"//g'`

#   Typical environment variables:
#   (NOTE some systems don't export these ENV vars. by default !!!)
#
#   Platform                        $OSTYPE      $MACHTYPE                $HOSTTYPE
#
#   Win2K [CMD.EXE]                 (none)       (none)                   (none)        
#
#   Cygwin Win2K [bash]             cygwin       i686-pc-cygwin           i686
#
#   OS X PPC 10.3 [zsh]             darwin7.0    powerpc                  (none)
#
#   OS X i386 10.4 [bash]           darwin8.0    i386-apple-darwin8.0     i386
#
#   OS X i386 10.5 [bash]           darwin9.0    i386-apple-darwin9.0     i386
#
#   OS X i386 10.6 [bash]           darwin10.0   x86_64-apple-darwin10.0  x86_64
#
#   OS X i386 10.7 [bash]           darwin11     x86_64-apple-darwin11    x86_64
#
#   OS X i386 10.12 [bash]          darwin16     x86_64-apple-darwin16    x86_64
#
#   OS X ARM macOS Tahoe [bash]     darwin25     arm64-apple-darwin25     arm64
#
#   Linux RH 4.0 [bash]             linux-gnu    i686-redhat-linux-gnu    i686
#
#   Linux Fedora 7.1 [bash]         linux-gnu    i386-redhat-linux-gnu    i386
#   Linux Ubuntu  ??7               linux-gnu    i486-pc-linux-gnu        i686
#
#   Linux Fedora 7.1 64 bit [bash]  linux-gnu    x86_64-redhat-linux-gnu  x86_64
#   Ubuntu 12.10 64 bit [bash]      linux-gnu    x86_64-pc-linux-gnu      x86_64
#
#   FreeBSD 9.1 64 bit [bash]       freebsd9.1   amd64-portbld-freebsd9.1 amd64
#

echo "About to make $PRODUCT binary distribution $VERSION"

TOPDIR=${PRODUCT}_V$VERSION

if [ X$OS != "XWindows_NT" ] ; then
	# Fixup issues with the .zip format
	chmod +x *.sh
fi

# Make sure that some environment variable are visible to Jam:
export OSTYPE MACHTYPE HOSTTYPE
unset USETARPREFIX

# Can't do parallel build on recent MS VC++
if [ X$VisualStudioVersion = "X17.0" ] ; then
	NUMBER_OF_PROCESSORS=1
fi

# Maybe we could get Jam to do the following ?

if [ X$OS = "XWindows_NT" ] ; then
	echo "We're on MSWindows!"
	# On Win 64 bit we need to check compiler target platform
	if [ X${COMPILER/MINGW64//} != X$COMPILER	\
	  -o X${COMPILER/MSVCPP64//} != X$COMPILER ] ; then
		echo "We're compiling to MSWin 64 bit !"
		PACKAGE=${PRODUCT}_V${VERSION}_win64_exe.zip
		USBDIRS="usb"
		USBBINFILES="binfiles.msw"
		unset USETAR
		WIN_TARG="64"
	else if [ X${COMPILER/MSVCPP_ARM64//} != X$COMPILER ] ; then
		echo "We're compiling to ARM 64 bit !"
		PACKAGE=${PRODUCT}_V${VERSION}_win_arm64_exe.zip
		USBDIRS="usb"
		USBBINFILES="binfiles.msw"
		WIN_TARG="64"
		CROSS_COMPILE="yes"		# Assume this is so. Could make this more intelligent...
		unset USETAR
	else if [ X${COMPILER/MINGW//} != X$COMPILER	\
	       -o X${COMPILER/MSVCPP//} != X$COMPILER ] ; then
		echo "We're compiling to MSWin 32 bit !"
		PACKAGE=${PRODUCT}_V${VERSION}_win32_exe.zip
		USBDIRS="usb"
		USBBINFILES="binfiles.msw"
		WIN_TARG="32"
		unset USETAR
	fi
	fi
	fi
else if [ "${OSTYPE#*darwin*}" != "$OSTYPE" ] ; then
	if [ X$OSTYPE = "Xdarwin7.0" ] ; then
		echo "We're on OSX 10.3 PPC!"
		PACKAGE=${PRODUCT}_V${VERSION}_osx10.3_ppc_bin.tgz
	else if [ X$MACHTYPE = "Xpowerpc-apple-darwin8.0" ] ; then
		echo "We're on OSX 10.4 PPC!"
		PACKAGE=${PRODUCT}_V${VERSION}_osx10.4_ppc_bin.tgz
	else if [ X$MACHTYPE = "Xi386-apple-darwin8.0" \
		  -o X$COMPILER = "XOSX10_4_X86_32BIT" ] ; then
		echo "We're on OSX 10.4 i386!"
		PACKAGE=${PRODUCT}_V${VERSION}_osx10.4_i86_bin.tgz
	else if [ X$HOSTTYPE = "Xx86_64" \
		  -o X$COMPILER = "XOSX10_6_X86_64BIT" ] ; then
		echo "We're on OSX 10 x86_64!"
		PACKAGE=${PRODUCT}_V${VERSION}_osx10.6_x86_64_bin.tgz
	else if [ X$HOSTTYPE = "Xarm64" ] ; then
		echo "We're on MacOS arm64!"
		PACKAGE=${PRODUCT}_V${VERSION}_macOS_arm64_bin.tgz
		export MACOSX_DEPLOYMENT_TARGET="11.0"	# Minimum target platform version
	fi
	fi
	fi
	fi
	fi
	USBDIRS="usb"
	USBBINFILES="binfiles.osx"
	USETAR=true
	USETARPREFIX=true
else if [ X$OSTYPE = "Xlinux-gnu" ] ; then
	if [[ "$MACHTYPE" = x86_64-*-linux-gnu ]] ; then
		echo "We're on Linux x86_64!"
		PACKAGE=${PRODUCT}_V${VERSION}_linux_x86_64_bin.tgz
	else if [[ "$MACHTYPE" = *86-*-linux-gnu ]] ; then
		echo "We're on Linux x86!"
		PACKAGE=${PRODUCT}_V${VERSION}_linux_x86_bin.tgz
	fi
	fi
	USBDIRS="usb"
	USBBINFILES="binfiles.lx"
	USETAR=true
fi
fi
fi

if [ X$PACKAGE = "X" ] ; then
	echo "Unknown host - build failed!"
	exit 1
fi 

echo "Making GNU $PRODUCT binary distribution $PACKAGE for Version $VERSION"

# Clean up so we get a solid build
# .sp come from profile, .cht from scanin and .ti3 from spectro
rm -f bin/*.exe bin/*.dll
rm -f ref/*.sp ref/*.cht ref/*.ti2

if [ X$CROSS_COMPILE = "X" ] ; then		# If not cross compiling
  echo "Cleaning before build ..."
  if ! jam -fJambase -sBUILTIN_TIFF=true -sBUILTIN_JPEG=true -sBUILTIN_PNG=true -sBUILTIN_Z=true -sBUILTIN_SSL=true clean ; then
    	echo "Clean failed!"
  	exit 1
  fi 
fi

# Make sure it's built and installed
if ! jam -q -fJambase -j${NUMBER_OF_PROCESSORS:-1} -sBUILTIN_TIFF=true -sBUILTIN_JPEG=true -sBUILTIN_PNG=true -sBUILTIN_Z=true -sBUILTIN_SSL=true install ; then
	echo "Build failed!"
	exit 1
fi 

rm -rf $TOPDIR
mkdir $TOPDIR

# Collect the names of all the files that we're going to package
unset topfiles; for i in `cat binfiles`; do topfiles="$topfiles ${i}"; done
unset docfiles; for i in `cat doc/afiles`; do docfiles="$docfiles doc/${i}"; done
unset usbfiles;
for j in ${USBDIRS}; do
	if [ ${j} ]; then
		for i in `cat ${j}/${USBBINFILES}`; do usbfiles="$usbfiles ${j}/${i}"; done
	fi
done

allfiles="${topfiles} bin/* ref/* ${docfiles} ${usbfiles}"

# Copy all the files to the package top directory
for i in ${allfiles}; do
	path=${i%/*}		# extract path without filename
	file=${i##*/}		# extract filename
	if [ $path = $i ] ; then
		path=
	fi
	if [ X$path != "X" ] ; then
		mkdir -p $TOPDIR/${path}
	fi
	if [ X${file} = "Xafiles" ] ; then
		continue
	fi
	cp $i $TOPDIR/$i
done

# Create the package
rm -f $PACKAGE
if [ X$USETAR = "Xtrue" ] ; then
	if [ X$USETARPREFIX = "Xtrue" ] ; then
		# Don't save ._* files...
		COPYFILE_DISABLE=1 tar -czvf $PACKAGE $TOPDIR
	else
		tar -czvf $PACKAGE $TOPDIR
	fi
	# tar -xzvf to extract
	# tar -tzf to list
	# to update a file:
	#  gzip -d archive.tgz
	#  tar -uf application.tar file
	#  gzip application.tar
	#  mv application.tar.gz application.tgz
	# or "tgzupdate.sh application fullpath/file1 fullpath/file2 fullpath/file3"
	# Should we use "COPYFILE_DISABLE=1 tar .." on OS X ??
else
	zip -9 -r $PACKAGE $TOPDIR
	# unzip to extract
	# unzip -l to list
	# zip archive.zip path/file to update
fi
rm -rf $TOPDIR
echo "Done GNU $PRODUCT binary distribution $PACKAGE"

echo "Remember to:"
if [ X$USETAR = "Xtrue" ] ; then
    echo "   ftp 10.0.0.1; put $PACKAGE"
else
    echo "   cp $PACKAGE ~/ftp"
fi

exit 0

