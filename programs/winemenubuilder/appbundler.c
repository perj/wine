/*
 * Winemenubuilder support for Mac OS X Application Bundles
 *
 * Copyright 2011 Steven Edwards
 * Copyright 2011 - 2012 Per Johansson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 * NOTES: An Application Bundle generally has the following layout
 *
 * foo.app/Contents
 * foo.app/Contents/Info.plist
 * foo.app/Contents/MacOS/foo (can be script or real binary)
 * foo.app/Contents/Resources/appIcon.icns (Apple Icon format)
 * foo.app/Contents/Resources/English.lproj/infoPlist.strings
 * foo.app/Contents/Resources/English.lproj/MainMenu.nib (Menu Layout)
 *
 * There can be more to a bundle depending on the target, what resources
 * it contains and what the target platform but this simplifed format
 * is all we really need for now for Wine.
 *
 * TODO:
 * - Convert to using CoreFoundation API rather than standard unix file ops
 * - See if there is anything else in the rsrc section of the target that
 *   we might want to dump in a *.plist. Version information for the target
 *   and or Wine Version information come to mind.
 * - sha1hash of target application in bundle plist
 */

#ifdef __APPLE__

#include "config.h"
#include "wine/port.h"

#include <stdio.h>
#include <errno.h>

#include <CoreFoundation/CoreFoundation.h>
#ifdef HAVE_APPLICATIONSERVICES_APPLICATIONSERVICES_H
#define GetCurrentProcess GetCurrentProcess_Mac
#define GetCurrentThread GetCurrentThread_Mac
#define LoadResource LoadResource_Mac
#define EqualRect EqualRect_Mac
#define FillRect FillRect_Mac
#define FrameRect FrameRect_Mac
#define GetCursor GetCursor_Mac
#define InvertRect InvertRect_Mac
#define OffsetRect OffsetRect_Mac
#define PtInRect PtInRect_Mac
#define SetCursor SetCursor_Mac
#define SetRect SetRect_Mac
#define ShowCursor ShowCursor_Mac
#define UnionRect UnionRect_Mac
#define Polygon Polygon_Mac
#include <ApplicationServices/ApplicationServices.h>
#undef GetCurrentProcess
#undef GetCurrentThread
#undef LoadResource
#undef EqualRect
#undef FillRect
#undef FrameRect
#undef GetCursor
#undef InvertRect
#undef OffsetRect
#undef PtInRect
#undef SetCursor
#undef SetRect
#undef ShowCursor
#undef UnionRect
#undef Polygon
#undef DPRINTF
#endif

#define COBJMACROS
#define NONAMELESSUNION

#include <windows.h>
#include <shlobj.h>
#include <objidl.h>
#include <shlguid.h>
#include <appmgmt.h>
#include <tlhelp32.h>
#include <intshcut.h>
#include <shlwapi.h>
#include <initguid.h>

#include "wine/debug.h"
#include "wine/library.h"

#include "winemenubuilder.h"


WINE_DEFAULT_DEBUG_CHANNEL(menubuilder);

static char *mac_desktop_dir = NULL;
static char *wine_applications_dir = NULL;
static char *wine_associations_dir = NULL;

DEFINE_GUID(CLSID_WICIcnsEncoder, 0x312fb6f1,0xb767,0x409d,0x8a,0x6d,0x0f,0xc1,0x54,0xd4,0xf0,0x5c);

#define ICNS_SLOTS 6

static char *strdupA( const char *str )
{
    char *ret;

    if (!str) return NULL;
    if ((ret = HeapAlloc( GetProcessHeap(), 0, strlen(str) + 1 ))) strcpy( ret, str );
    return ret;
}

static inline int size_to_slot(int size)
{
    switch (size)
    {
        case 16: return 0;
        case 32: return 1;
        case 48: return 2;
        case 128: return 3;
        case 256: return 4;
        case 512: return 5;
    }

    return -1;
}

HRESULT WriteBundleIcon(IStream *icoStream, int exeIndex, LPCWSTR icoPathW,
        const char *destFilename, char *icnsName)
{
    ICONDIRENTRY *iconDirEntries = NULL;
    int numEntries;
    struct {
        int index;
        int maxBits;
    } best[ICNS_SLOTS];
    int indexes[ICNS_SLOTS];
    int i;
    LARGE_INTEGER zero;
    HRESULT hr;

    hr = read_ico_direntries(icoStream, &iconDirEntries, &numEntries);
    if (FAILED(hr))
        goto end;
    for (i = 0; i < ICNS_SLOTS; i++)
    {
        best[i].index = -1;
        best[i].maxBits = 0;
    }
    for (i = 0; i < numEntries; i++)
    {
        int slot;
        int width = iconDirEntries[i].bWidth ? iconDirEntries[i].bWidth : 256;
        int height = iconDirEntries[i].bHeight ? iconDirEntries[i].bHeight : 256;

        WINE_TRACE("[%d]: %d x %d @ %d\n", i, width, height, iconDirEntries[i].wBitCount);
        if (height != width)
            continue;
        slot = size_to_slot(width);
        if (slot < 0)
            continue;
        if (iconDirEntries[i].wBitCount >= best[slot].maxBits)
        {
            best[slot].index = i;
            best[slot].maxBits = iconDirEntries[i].wBitCount;
        }
    }
    numEntries = 0;
    for (i = 0; i < ICNS_SLOTS; i++)
    {
        if (best[i].index >= 0)
        {
            indexes[numEntries] = best[i].index;
            numEntries++;
        }
    }

    zero.QuadPart = 0;
    hr = IStream_Seek(icoStream, zero, STREAM_SEEK_SET, NULL);
    if (FAILED(hr))
    {
        WINE_WARN("seeking icon stream failed, error 0x%08X\n", hr);
        goto end;
    }
    hr = convert_to_native_icon(icoStream, indexes, numEntries, &CLSID_WICIcnsEncoder,
            icnsName, icoPathW);
    if (FAILED(hr))
    {
        WINE_WARN("converting %s to %s failed, error 0x%08X\n",
                wine_dbgstr_w(icoPathW), wine_dbgstr_a(icnsName), hr);
        goto end;
    }

end:
    HeapFree(GetProcessHeap(), 0, iconDirEntries);
    return hr;
}

static HRESULT CreateIconIdentifier(LPCWSTR icoPathW, char **nativeIdentifier)
{
    char *str, *p, *q;

    str = wchars_to_utf8_chars(icoPathW);
    p = strrchr(str, '\\');
    if (p == NULL)
        p = str;
    else
    {
        *p = 0;
        p++;
    }
    q = strrchr(p, '.');
    if (q)
        *q = 0;
    *nativeIdentifier = heap_printf("%s.icns", p);

    HeapFree(GetProcessHeap(), 0, str);
    return S_OK;
}

HRESULT appbundle_write_icon(IStream *icoStream, int exeIndex, LPCWSTR icoPathW,
        const char *destFilename, char **nativeIdentifier)
{
    if (*nativeIdentifier)
        return WriteBundleIcon(icoStream, exeIndex, icoPathW, destFilename, *nativeIdentifier);
    return CreateIconIdentifier(icoPathW, nativeIdentifier);
}

CFDictionaryRef CreateMyDictionary(const char *pathname, const char *linkname, const char *icon)
{
    CFMutableDictionaryRef dict;
    CFStringRef pathstr;
    CFStringRef linkstr;
    CFStringRef idstr;

    pathstr = CFStringCreateWithCString(NULL, pathname, CFStringGetSystemEncoding());
    linkstr = CFStringCreateWithCString(NULL, linkname, CFStringGetSystemEncoding());
    idstr = CFStringCreateWithFormat(NULL, NULL, CFSTR("org.winehq.wine.%@"), linkstr);

    /* Create a dictionary that will hold the data. */
    dict = CFDictionaryCreateMutable( kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks );

    /* Put the various items into the dictionary. */
    /* FIXME - Some values assumed the ought not to be */
    CFDictionarySetValue( dict, CFSTR("CFBundleDevelopmentRegion"), CFSTR("English") );
    CFDictionarySetValue( dict, CFSTR("CFBundleExecutable"), linkstr );
    /* FIXME - Avoid identifier if not unique. */
    //CFDictionarySetValue( dict, CFSTR("CFBundleIdentifier"), idstr );
    CFDictionarySetValue( dict, CFSTR("CFBundleInfoDictionaryVersion"), CFSTR("6.0") );
    CFDictionarySetValue( dict, CFSTR("CFBundleName"), linkstr );
    CFDictionarySetValue( dict, CFSTR("CFBundleDisplayName"), pathstr );
    CFDictionarySetValue( dict, CFSTR("CFBundlePackageType"), CFSTR("APPL") );
    CFDictionarySetValue( dict, CFSTR("CFBundleVersion"), CFSTR("1.0") );
    //CFDictionarySetValue( dict, CFSTR("LSHasLocalizedDisplayName"), kCFBooleanFalse );
    // Not needed CFDictionarySetValue( dict, CFSTR("CFBundleSignature"), CFSTR("????") );
    /* Fixme - install a default icon */
    //CFDictionarySetValue( dict, CFSTR("CFBundleIconFile"), CFSTR("wine.icns") );

    if (icon)
    {
	CFStringRef iconstr = CFStringCreateWithCString(NULL, icon, CFStringGetSystemEncoding());

	CFDictionarySetValue( dict, CFSTR("CFBundleIconFile"), iconstr );
	CFRelease(iconstr);
    }

    CFRelease(linkstr);
    CFRelease(idstr);

    return dict;
}

void WriteMyPropertyListToFile( CFPropertyListRef propertyList, CFURLRef fileURL, CFPropertyListFormat format)
{
    CFDataRef data;
    Boolean status;
    SInt32 errorCode;
    CFErrorRef err = NULL;

    /* Convert the property list into XML data */
    data = CFPropertyListCreateData(NULL, propertyList, format, 0, &err);

    /* Write the XML data to the file */
    status = CFURLWriteDataAndPropertiesToResource (
            fileURL,
            data,
            NULL,
            &errorCode);

    // CFRelease(xmlData);
}

static BOOL generate_plist(const char *path_to_bundle_contents, const char *pathname, const char *linkname, const char *icon)
{
    char *plist_path;
    static const char info_dot_plist_file[] = "Info.plist";
    CFPropertyListRef propertyList;
    CFStringRef pathstr;
    CFURLRef fileURL;

    /* Append all of the filename and path stuff and shove it in to CFStringRef */
    plist_path = heap_printf("%s/%s", path_to_bundle_contents, info_dot_plist_file);
    pathstr = CFStringCreateWithCString(NULL, plist_path, CFStringGetSystemEncoding());

    /* Construct a complex dictionary object */
    propertyList = CreateMyDictionary(pathname, linkname, icon);

    /* Create a URL that specifies the file we will create to hold the XML data. */
    fileURL = CFURLCreateWithFileSystemPath( kCFAllocatorDefault,
            pathstr,
            kCFURLPOSIXPathStyle,
            false );

    /* Write the property list to the file */
    WriteMyPropertyListToFile( propertyList, fileURL, kCFPropertyListXMLFormat_v1_0 );
    CFRelease(propertyList);

#if 0
    /* Recreate the property list from the file */
    propertyList = CreateMyPropertyListFromFile( fileURL );

    /* Release any objects to which we have references */
    CFRelease(propertyList);
#endif
    CFRelease(fileURL);

    WINE_TRACE("Creating Bundle Info.plist at %s\n", wine_dbgstr_a(plist_path));

    return TRUE;
}

CFDictionaryRef CreateStringsDictionary(const char *linkname)
{
    CFMutableDictionaryRef dict;
    CFStringRef linkstr;

    linkstr = CFStringCreateWithCString(NULL, linkname, CFStringGetSystemEncoding());

    /* Create a dictionary that will hold the data. */
    dict = CFDictionaryCreateMutable( kCFAllocatorDefault,
            1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks );

    CFDictionarySetValue( dict, CFSTR("CFBundleDisplayName"), linkstr );

    CFRelease(linkstr);

    return dict;
}

static BOOL generate_plist_strings(const char *path_to_bundle_resources_lang, const char *linkname)
{
    char *strings_path;
    static const char strings_file[] = "InfoPlist.strings";
    CFPropertyListRef propertyList;
    CFStringRef pathstr;
    CFURLRef fileURL;

    /* Append all of the filename and path stuff and shove it in to CFStringRef */
    strings_path = heap_printf("%s/%s", path_to_bundle_resources_lang, strings_file);
    pathstr = CFStringCreateWithCString(NULL, strings_path, CFStringGetSystemEncoding());

    /* Construct a complex dictionary object */
    propertyList = CreateStringsDictionary(linkname);

    /* Create a URL that specifies the file we will create to hold the XML data. */
    fileURL = CFURLCreateWithFileSystemPath( kCFAllocatorDefault,
            pathstr,
            kCFURLPOSIXPathStyle,
            false );

    /* Write the property list to the file */
    WriteMyPropertyListToFile( propertyList, fileURL, kCFPropertyListBinaryFormat_v1_0 );
    CFRelease(propertyList);

    CFRelease(fileURL);

    WINE_TRACE("Creating InfoPlist.strings at %s\n", wine_dbgstr_a(strings_path));

    return TRUE;
}


#if 0
/* TODO: If I understand this file correctly, it is used for associations */
static BOOL generate_pkginfo_file(const char* path_to_bundle_contents)
{
    FILE *file;
    char *bundle_and_pkginfo;
    static const char pkginfo_file[] = "PkgInfo";

    bundle_and_pkginfo = heap_printf("%s/%s", path_to_bundle_contents, pkginfo_file);

    WINE_TRACE("Creating Bundle PkgInfo at %s\n", wine_dbgstr_a(bundle_and_pkginfo));

    file = fopen(bundle_and_pkginfo, "w");
    if (file == NULL)
        return FALSE;

    fprintf(file, "APPL????");

    fclose(file);
    return TRUE;
}
#endif


/* inspired by write_desktop_entry() in xdg support code */
static BOOL generate_bundle_script(const char *path_to_bundle_macos, const char *path,
        const char *args, const char *workdir, const char *linkname)
{
    FILE *file;
    char *bundle_and_script;
    const char *libpath;

    bundle_and_script = heap_printf("%s/%s", path_to_bundle_macos, linkname);

    WINE_TRACE("Creating Bundle helper script at %s\n", wine_dbgstr_a(bundle_and_script));

    file = fopen(bundle_and_script, "w");
    if (file == NULL)
        return FALSE;

    fprintf(file, "#!/bin/sh\n");
    fprintf(file, "#Helper script for %s\n\n", linkname);

    fprintf(file, "PATH=\"%s\"\nexport PATH\n", getenv("PATH"));
    libpath = getenv("DYLD_FALLBACK_LIBRARY_PATH");
    if (libpath)
        fprintf(file, "DYLD_FALLBACK_LIBRARY_PATH=\"%s\"\nexport DYLD_FALLBACK_LIBRARY_PATH\n", libpath);
    fprintf(file, "WINEPREFIX=\"%s\"\nexport WINEPREFIX\n\n", wine_get_config_dir());

    if (workdir)
        fprintf(file, "cd \"%s\"\n", workdir);
    fprintf(file, "exec sh -c \"exec wine %s %s\"\n\n", path, args);

    fprintf(file, "#EOF\n");

    fclose(file);
    chmod(bundle_and_script, 0755);

    return TRUE;
}

/* build out the directory structure for the bundle and then populate */
BOOL build_app_bundle(const char *unix_link, const char *path, const char *args, const char *workdir, const char *dir, const char *link, const char *linkname, char **icon, CFPropertyListRef *infoplist)
{
    BOOL ret = FALSE;
    char *path_to_bundle, *bundle_name, *path_to_bundle_contents, *path_to_bundle_macos;
    char *path_to_bundle_resources, *path_to_bundle_resources_lang;
    static const char extentsion[] = "app";
    static const char contents[] = "Contents";
    static const char macos[] = "MacOS";
    static const char resources[] = "Resources";
    static const char resources_lang[] = "English.lproj"; /* FIXME */

    WINE_TRACE("bundle file name %s\n", wine_dbgstr_a(linkname));

    bundle_name = heap_printf("%s.%s", link, extentsion);
    path_to_bundle = heap_printf("%s/%s", dir, bundle_name);
    path_to_bundle_contents = heap_printf("%s/%s", path_to_bundle, contents);
    path_to_bundle_macos =  heap_printf("%s/%s", path_to_bundle_contents, macos);
    path_to_bundle_resources = heap_printf("%s/%s", path_to_bundle_contents, resources);
    path_to_bundle_resources_lang = heap_printf("%s/%s", path_to_bundle_resources, resources_lang);

    create_directories(path_to_bundle);
    create_directories(path_to_bundle_contents);
    create_directories(path_to_bundle_macos);
    create_directories(path_to_bundle_resources);
    create_directories(path_to_bundle_resources_lang);

    WINE_TRACE("created bundle %s\n", wine_dbgstr_a(path_to_bundle));

    ret = generate_bundle_script(path_to_bundle_macos, path, args, workdir, linkname);
    if(ret==FALSE)
        return ret;

#if 0
    ret = generate_pkginfo_file(path_to_bundle_contents);
    if(ret==FALSE)
        return ret;
#endif

    if (infoplist)
        *infoplist = CreateMyDictionary(link, linkname, *icon);
    else
    {
        ret = generate_plist(path_to_bundle_contents, link, linkname, *icon);
        if(ret==FALSE)
            return ret;
    }

    ret = generate_plist_strings(path_to_bundle_resources_lang, linkname);
    if (ret == FALSE)
        return ret;

    if (unix_link)
    {
        DWORD ret = register_menus_entry(path_to_bundle, unix_link);
        if (ret != ERROR_SUCCESS)
            return FALSE;
    }

    if (*icon)
    {
	char *tmp = heap_printf("%s/%s", path_to_bundle_resources, *icon);
	HeapFree(GetProcessHeap(), 0, *icon);
	*icon = tmp;
    }

    return TRUE;
}

int appbundle_build_desktop_link(const char *unix_link, const char *link, const char *link_name, const char *path,
        const char *args, const char *descr, const char *workdir, char **icon)
{
    return !build_app_bundle(unix_link, path, args, workdir, mac_desktop_dir, link_name, link_name, icon, NULL);
}

int appbundle_build_menu_link(const char *unix_link, const char *link, const char *link_name, const char *path,
        const char *args, const char *descr, const char *workdir, char **icon)
{
    return !build_app_bundle(unix_link, path, args, workdir, wine_applications_dir, link, link_name, icon, NULL);
}

void *appbundle_refresh_file_type_associations_init(void)
{
    static int ok;

    /* Noop */
    return &ok;
}

static CFStringRef find_uti_for_tag(CFStringRef tagClass, const char *tag)
{
    CFStringRef uti = NULL;
    CFStringRef tagStr = CFStringCreateWithCStringNoCopy(NULL, tag, kCFStringEncodingUTF8, kCFAllocatorNull);

    if (tagStr)
        uti = UTTypeCreatePreferredIdentifierForTag(tagClass, tagStr, NULL);
    if (uti && CFStringCompareWithOptions(uti, CFSTR("dyn."), CFRangeMake(0, 4), 0) == kCFCompareEqualTo)
    {
        CFRelease(uti);
        uti = NULL;
    }

    if (tagStr)
        CFRelease(tagStr);
    return uti;
}

static CFMutableDictionaryRef document_type_dictionary(CFStringRef uti, const char *icon)
{
    CFStringRef iconStr = icon ? CFStringCreateWithCString(NULL, icon, kCFStringEncodingUTF8) : NULL;
    CFMutableDictionaryRef res;
    CFMutableArrayRef utis;

    res = CFDictionaryCreateMutable(NULL, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    if (icon)
    	CFDictionarySetValue(res, CFSTR("CFBundleTypeIconFile"), iconStr);

    CFDictionarySetValue(res, CFSTR("CFBundleTypeName"), uti);
    
    /* XXX Viewer? Shell? */
    CFDictionarySetValue(res, CFSTR("CFBundleTypeRole"), CFSTR("Editor"));
    utis = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(utis, uti);
    CFDictionarySetValue(res, CFSTR("LSItemContentTypes"), utis);
    CFRelease(utis);
    CFDictionarySetValue(res, CFSTR("LSHandlerRank"), CFSTR("Alternate"));
    
    if (iconStr)
        CFRelease(iconStr);

    return res;
}

BOOL replace_document_type(CFPropertyListRef propertyList, CFStringRef uti, CFDictionaryRef dict)
{
    CFMutableArrayRef docs = (CFMutableArrayRef)CFDictionaryGetValue(propertyList, CFSTR("CFBundleDocumentTypes"));
    CFIndex count;
    CFIndex i;

    if (docs)
        count = CFArrayGetCount(docs);
    else
    {
        docs = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
        count = 0;
        CFDictionarySetValue((CFMutableDictionaryRef)propertyList, CFSTR("CFBundleDocumentTypes"), docs);
    }

    for ( i = 0 ; i < count ; i++)
    {
        CFDictionaryRef d = CFArrayGetValueAtIndex(docs, i);
        CFStringRef itemUti = CFDictionaryGetValue(d, CFSTR("CFBundleTypeName"));

        if (CFEqual(uti, itemUti))
            break;
    }

    if (i < count)
    {
        if (dict)
            CFArrayReplaceValues(docs, CFRangeMake(i, 1), (const void*[]){ dict }, 1);
        else
            CFArrayRemoveValueAtIndex(docs, i);
    }
    else if (docs)
        CFArrayAppendValue(docs, dict);
    return TRUE;
}


static CFMutableDictionaryRef exported_uti_dictionary(CFStringRef uti, const char *description, const char *icon,
        const char *extension, const char *mime_type)
{
    CFStringRef descStr = description ? CFStringCreateWithCString(NULL, description, kCFStringEncodingUTF8) : NULL;
    CFStringRef iconStr = icon ? CFStringCreateWithCString(NULL, icon, kCFStringEncodingUTF8) : NULL;
    CFStringRef extStr = CFStringCreateWithCString(NULL, extension, kCFStringEncodingUTF8);
    CFStringRef mimeStr = CFStringCreateWithCString(NULL, mime_type, kCFStringEncodingUTF8);
    CFDictionaryRef utidict = UTTypeCopyDeclaration(uti);
    CFMutableDictionaryRef res;
    CFDictionaryRef tagdict;

    if (utidict)
    {
        res = CFDictionaryCreateMutableCopy(NULL, 5, utidict);
        CFRelease(utidict);
        CFDictionaryRemoveValue(res, kUTTypeReferenceURLKey);
    }
    else
    {
        CFArrayRef conformsTo;

        /* Have to create from scratch. */
        res = CFDictionaryCreateMutable(NULL, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        CFDictionarySetValue(res, kUTTypeIdentifierKey, uti);

        if (strncmp(mime_type, "image/", 6) == 0)
            conformsTo = CFArrayCreate(NULL, (const void *[]){ CFSTR("public.image"), CFSTR("public.item") }, 2, &kCFTypeArrayCallBacks);
        else if (strncmp(mime_type, "text/", 5) == 0)
            conformsTo = CFArrayCreate(NULL, (const void *[]){ CFSTR("public.text"), CFSTR("public.item") }, 2, &kCFTypeArrayCallBacks);
        else
            conformsTo = CFArrayCreate(NULL, (const void *[]){ CFSTR("public.data"), CFSTR("public.item") }, 2, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(res, kUTTypeConformsToKey, conformsTo);
        CFRelease(conformsTo);
    }

    if (description)
    	CFDictionarySetValue(res, kUTTypeDescriptionKey, descStr);
    else
        CFDictionaryRemoveValue(res, kUTTypeDescriptionKey);
    if (icon)
        CFDictionarySetValue(res, kUTTypeIconFileKey, iconStr);
    else
        CFDictionaryRemoveValue(res, kUTTypeIconFileKey);
    
    tagdict = CFDictionaryCreate(NULL, (const void *[]){ CFSTR("public.filename-extension"), CFSTR("public.mime-type") },
            (const void *[]){ extStr, mimeStr }, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(res, kUTTypeTagSpecificationKey, tagdict);
    CFRelease(tagdict);

    if (descStr)
    	CFRelease(descStr);
    if (iconStr)
        CFRelease(iconStr);
    CFRelease(extStr);
    CFRelease(mimeStr);

    return res;
}

BOOL replace_exported_uti(CFPropertyListRef propertyList, CFStringRef uti, CFDictionaryRef dict)
{
    CFMutableArrayRef utis = (CFMutableArrayRef)CFDictionaryGetValue(propertyList, kUTExportedTypeDeclarationsKey);
    CFIndex count;
    CFIndex i;

    if (utis)
        count = CFArrayGetCount(utis);
    else
    {
        utis = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
        count = 0;
        CFDictionarySetValue((CFMutableDictionaryRef)propertyList, kUTExportedTypeDeclarationsKey, utis);
    }

    for ( i = 0 ; i < count ; i++)
    {
        CFDictionaryRef d = CFArrayGetValueAtIndex(utis, i);
        CFStringRef itemUti = CFDictionaryGetValue(d, kUTTypeIdentifierKey);

        if (CFEqual(uti, itemUti))
            break;
    }

    if (i < count)
    {
        if (dict)
            CFArrayReplaceValues(utis, CFRangeMake(i, 1), (const void*[]){ dict }, 1);
        else
            CFArrayRemoveValueAtIndex(utis, i);
    }
    else if (dict)
        CFArrayAppendValue(utis, dict);
    return TRUE;
}

BOOL appbundle_mime_type_for_extension(void *user, const char *extensionA, LPCWSTR extensionW, char **mime_type)
{
    CFStringRef uti = NULL;
    CFStringRef mime = NULL;
    BOOL ret = TRUE;

    uti = find_uti_for_tag(kUTTagClassFilenameExtension, &extensionA[1]);
    if (uti)
        mime = UTTypeCopyPreferredTagWithClass(uti, kUTTagClassMIMEType);

    if (mime) {
        char buf[1024];

        if (CFStringGetCString(mime, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            *mime_type = strdupA(buf);
        }
    }

    if (uti)
        CFRelease(uti);
    if (mime)
        CFRelease(mime);
    return ret;
}

BOOL appbundle_write_mime_type_entry(void *user, const char *extensionA, const char *mimeTypeA, const char *friendlyDocNameA)
{
    /* Noop */
    return TRUE;
}

BOOL appbundle_write_association_entry(void *user, const char *extensionA, const char *friendlyAppNameA,
		const char *friendlyDocNameA, const char *mimeTypeA, const char *progIdA,
                char **appIconA, char **docIconA)
{
    char *bundle_name = heap_printf("wine-extension-%s", &extensionA[1]);
    char *plist_path;
    CFURLRef fileURL;
    CFMutableDictionaryRef dict;
    CFStringRef pathstr;
    CFPropertyListRef propertyList;
    CFStringRef uti;
    char utibuf[256];
    CFStringRef winePrefix = CFStringCreateWithCString(NULL, wine_get_config_dir(), CFStringGetSystemEncoding());
    CFStringRef progIdStr = CFStringCreateWithCString(NULL, progIdA, CFStringGetSystemEncoding());
    char *args;
    char *path_to_bundle;

    WINE_TRACE("enter extensionA = %s friendlyAppNameA = %s friendlyDocNameA = %s mimeTypeA = %s progIdA = %s appIconA = %s docIconA = %s\n", extensionA, friendlyAppNameA, friendlyDocNameA, mimeTypeA, progIdA, *appIconA, *docIconA);

    path_to_bundle = heap_printf("%s/%s.app", wine_associations_dir, bundle_name);
    plist_path = heap_printf("%s/Contents/Info.plist", path_to_bundle);

    args = heap_printf("/AppleEvent /ProgIDOpen %s", progIdA);
    WINE_TRACE("new bundle %s\n", path_to_bundle);
    build_app_bundle(NULL, "start", args, NULL, wine_associations_dir, bundle_name, friendlyAppNameA, appIconA, &propertyList);
    HeapFree(GetProcessHeap(), 0, args);

    pathstr = CFStringCreateWithCString(NULL, plist_path, CFStringGetSystemEncoding());
    /* Create a URL that specifies the file we will create to hold the XML data. */
    fileURL = CFURLCreateWithFileSystemPath( kCFAllocatorDefault,
            pathstr,
            kCFURLPOSIXPathStyle,
            false );

    CFDictionaryAddValue((CFMutableDictionaryRef)propertyList, CFSTR("org.winehq.wineprefix"), winePrefix);
    CFDictionaryAddValue((CFMutableDictionaryRef)propertyList, CFSTR("org.winehq.progid"), progIdStr);

    uti = find_uti_for_tag(kUTTagClassMIMEType, mimeTypeA);
    if (!uti)
        uti = find_uti_for_tag(kUTTagClassFilenameExtension, &extensionA[1]);
    if (!uti)
        uti = CFStringCreateWithFormat(NULL, NULL, CFSTR("org.winehq.extension%s"), extensionA);
    CFStringGetCString(uti, utibuf, sizeof(utibuf), kCFStringEncodingUTF8);
    WINE_TRACE("uti = %s\n", utibuf);

    dict = exported_uti_dictionary(uti, friendlyDocNameA, *docIconA, &extensionA[1], mimeTypeA);
    replace_exported_uti(propertyList, uti, dict);
    CFRelease(dict);

    dict = document_type_dictionary(uti, *docIconA);
    replace_document_type(propertyList, uti, dict);
    CFRelease(dict);

    WriteMyPropertyListToFile( propertyList, fileURL, kCFPropertyListXMLFormat_v1_0 );

    /* Update docIcon to full path. App icon is handled by build_app_bundle */
    if (*docIconA)
    {
	char *tmp = heap_printf("%s/Contents/Resources/%s", path_to_bundle, *docIconA);
	HeapFree(GetProcessHeap(), 0, *docIconA);
	*docIconA = tmp;
    }

    {
        CFStringRef pathstr = CFStringCreateWithCString(NULL, path_to_bundle, CFStringGetSystemEncoding());
        CFURLRef bundleURL = CFURLCreateWithFileSystemPath( kCFAllocatorDefault,
                pathstr,
                kCFURLPOSIXPathStyle,
                false );
        LSRegisterURL(bundleURL, true);
        CFRelease(bundleURL);
        CFRelease(pathstr);
    }

    WINE_TRACE("exit extensionA = %s friendlyAppNameA = %s friendlyDocNameA = %s mimeTypeA = %s progIdA = %s appIconA = %s docIconA = %s\n", extensionA, friendlyAppNameA, friendlyDocNameA, mimeTypeA, progIdA, *appIconA, *docIconA);

    return TRUE;
}

BOOL appbundle_remove_file_type_association(void *user, const char *extensionA, LPCWSTR extensionW)
{
    char *path_to_bundle = heap_printf("%s/wine-extension-%s.app", wine_associations_dir, &extensionA[1]);

    if (path_to_bundle)
    {
        WINE_TRACE("removing file type association for %s\n", wine_dbgstr_w(extensionW));
        remove_unix_link(path_to_bundle);
        HeapFree(GetProcessHeap(), 0, path_to_bundle);
        return TRUE;
    }

    return FALSE;
}

void appbundle_refresh_file_type_associations_cleanup(void *user, BOOL hasChanged)
{
}

BOOL appbundle_init(void)
{
    WCHAR shellDesktopPath[MAX_PATH];

    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_DESKTOP, NULL, SHGFP_TYPE_CURRENT, shellDesktopPath);
    if (SUCCEEDED(hr))
        mac_desktop_dir = wine_get_unix_file_name(shellDesktopPath);

    if (mac_desktop_dir == NULL)
    {
        WINE_ERR("error looking up the desktop directory\n");
        return FALSE;
    }

    if (getenv("HOME"))
    {
        wine_applications_dir = heap_printf("%s/Applications/Wine", getenv("HOME"));
        create_directories(wine_applications_dir);
        WINE_TRACE("%s\n", wine_applications_dir);

        wine_associations_dir = heap_printf("%s/Library/Wine/Associations", getenv("HOME"));
        create_directories(wine_associations_dir);
        WINE_TRACE("%s\n", wine_associations_dir);

        return TRUE;
    }
    else
    {
        WINE_ERR("out of memory\n");
        return FALSE;
    }
}

const struct winemenubuilder_dispatch appbundle_dispatch =
{
    appbundle_init,

    appbundle_build_desktop_link,
    appbundle_build_menu_link,

    appbundle_write_icon,

    appbundle_refresh_file_type_associations_init,
    appbundle_mime_type_for_extension,
    appbundle_write_mime_type_entry,
    appbundle_write_association_entry,
    appbundle_remove_file_type_association,
    appbundle_refresh_file_type_associations_cleanup
};

#endif