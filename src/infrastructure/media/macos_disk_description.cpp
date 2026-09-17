// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/macos_disk_description.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOKitLib.h>

#include <algorithm>
#include <cctype>

namespace seabass::infrastructure::media
{

namespace
{

// Owns one CoreFoundation reference (a Create/Copy result).
template<typename Ref>
class CFOwned
{
public:
    explicit CFOwned(Ref ref = nullptr) : m_ref(ref) {}
    ~CFOwned()
    {
        if (m_ref) {
            CFRelease(m_ref);
        }
    }
    CFOwned(const CFOwned &) = delete;
    CFOwned &operator=(const CFOwned &) = delete;
    Ref get() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }

private:
    Ref m_ref;
};

// Owns one IOKit object reference.
class IOOwned
{
public:
    explicit IOOwned(io_object_t object = IO_OBJECT_NULL) : m_object(object) {}
    ~IOOwned()
    {
        if (m_object != IO_OBJECT_NULL) {
            IOObjectRelease(m_object);
        }
    }
    IOOwned(const IOOwned &) = delete;
    IOOwned &operator=(const IOOwned &) = delete;
    io_object_t get() const { return m_object; }

private:
    io_object_t m_object;
};

std::string toStdString(CFStringRef string)
{
    if (!string) {
        return {};
    }
    if (const char *fast = CFStringGetCStringPtr(string, kCFStringEncodingUTF8)) {
        return fast;
    }
    CFIndex length = CFStringGetLength(string);
    CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(string, buffer.data(), capacity, kCFStringEncodingUTF8)) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

std::string stringValue(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        return {};
    }
    return toStdString(static_cast<CFStringRef>(value));
}

bool boolValue(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

std::uint64_t uint64Value(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return 0;
    }
    SInt64 number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number) || number < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(number);
}

std::string uuidValue(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value || CFGetTypeID(value) != CFUUIDGetTypeID()) {
        return {};
    }
    CFOwned<CFStringRef> text(CFUUIDCreateString(kCFAllocatorDefault, static_cast<CFUUIDRef>(value)));
    return toStdString(text.get());
}

std::string pathValue(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value || CFGetTypeID(value) != CFURLGetTypeID()) {
        return {};
    }
    CFOwned<CFStringRef> path(CFURLCopyFileSystemPath(static_cast<CFURLRef>(value), kCFURLPOSIXPathStyle));
    return toStdString(path.get());
}

// The USB serial lives on the USB device node, several registry levels
// above the IOMedia; searching the parents finds it wherever the bridge
// driver put it. Card readers and some cheap bridges have none.
std::string usbSerialNumber(io_service_t media)
{
    for (CFStringRef key : {CFSTR("USB Serial Number"), CFSTR("kUSBSerialNumberString")}) {
        CFOwned<CFTypeRef> value(IORegistryEntrySearchCFProperty(media, kIOServicePlane, key, kCFAllocatorDefault,
                                                                 kIORegistryIterateRecursively
                                                                     | kIORegistryIterateParents));
        if (value && CFGetTypeID(value.get()) == CFStringGetTypeID()) {
            std::string serial = toStdString(static_cast<CFStringRef>(value.get()));
            auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            serial.erase(serial.begin(), std::find_if(serial.begin(), serial.end(), notSpace));
            serial.erase(std::find_if(serial.rbegin(), serial.rend(), notSpace).base(), serial.end());
            if (!serial.empty()) {
                return serial;
            }
        }
    }
    return {};
}

bool publishedByBlockStorageDriver(io_service_t media)
{
    io_registry_entry_t parent = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentEntry(media, kIOServicePlane, &parent) != KERN_SUCCESS) {
        return false;
    }
    IOOwned owned(parent);
    return IOObjectConformsTo(parent, "IOBlockStorageDriver");
}

std::optional<MacDiskDescription> describe(DADiskRef disk)
{
    CFOwned<CFDictionaryRef> dict(DADiskCopyDescription(disk));
    if (!dict) {
        return std::nullopt;
    }

    MacDiskDescription result;
    result.bsdName = stringValue(dict.get(), kDADiskDescriptionMediaBSDNameKey);
    if (result.bsdName.empty()) {
        return std::nullopt;
    }
    result.whole = boolValue(dict.get(), kDADiskDescriptionMediaWholeKey);
    result.internal = boolValue(dict.get(), kDADiskDescriptionDeviceInternalKey);
    result.removable = boolValue(dict.get(), kDADiskDescriptionMediaRemovableKey)
        || boolValue(dict.get(), kDADiskDescriptionMediaEjectableKey);
    result.sizeBytes = uint64Value(dict.get(), kDADiskDescriptionMediaSizeKey);
    result.protocol = stringValue(dict.get(), kDADiskDescriptionDeviceProtocolKey);
    result.model = stringValue(dict.get(), kDADiskDescriptionDeviceModelKey);
    result.content = stringValue(dict.get(), kDADiskDescriptionMediaContentKey);
    result.volumeKind = stringValue(dict.get(), kDADiskDescriptionVolumeKindKey);
    result.volumeName = stringValue(dict.get(), kDADiskDescriptionVolumeNameKey);
    result.volumeUuid = uuidValue(dict.get(), kDADiskDescriptionVolumeUUIDKey);
    result.mountPoint = pathValue(dict.get(), kDADiskDescriptionVolumePathKey);

    // The whole disk's IOMedia is what tells a real drive from a
    // synthesized one, and what the USB serial hangs above.
    CFOwned<DADiskRef> wholeDisk(result.whole ? static_cast<DADiskRef>(const_cast<void *>(CFRetain(disk)))
                                              : DADiskCopyWholeDisk(disk));
    result.wholeBsdName = result.bsdName;
    if (wholeDisk) {
        if (const char *name = DADiskGetBSDName(wholeDisk.get())) {
            result.wholeBsdName = name;
        }
        IOOwned media(DADiskCopyIOMedia(wholeDisk.get()));
        if (media.get() != IO_OBJECT_NULL) {
            result.physical = publishedByBlockStorageDriver(media.get());
            result.usbSerial = usbSerialNumber(media.get());
        }
    }
    return result;
}

}  // namespace

bool isRemovableStickDisk(const MacDiskDescription &wholeDisk)
{
    if (!wholeDisk.whole || !wholeDisk.physical || wholeDisk.sizeBytes == 0) {
        return false;
    }
    if (wholeDisk.protocol == "USB") {
        return true;
    }
    // A built-in SDXC reader is "internal" as a device, but the card in it
    // is removable media -- that flag is what separates it from anything
    // soldered in.
    return wholeDisk.protocol == "Secure Digital" && wholeDisk.removable;
}

std::optional<std::string> bsdNameFromDevicePath(const std::string &devicePath)
{
    std::string name = devicePath;
    if (name.rfind("/dev/", 0) == 0) {
        name = name.substr(5);
    }
    if (name.rfind("disk", 0) != 0) {
        return std::nullopt;
    }
    // disk<digits> or disk<digits>s<digits>, nothing else.
    std::size_t i = 4;
    auto digitsFrom = [&name](std::size_t &pos) {
        std::size_t start = pos;
        while (pos < name.size() && std::isdigit(static_cast<unsigned char>(name[pos]))) {
            ++pos;
        }
        return pos > start;
    };
    if (!digitsFrom(i)) {
        return std::nullopt;
    }
    if (i < name.size()) {
        if (name[i] != 's') {
            return std::nullopt;
        }
        ++i;
        if (!digitsFrom(i) || i != name.size()) {
            return std::nullopt;
        }
    }
    return name;
}

std::vector<MacDiskDescription> describeAllDisks()
{
    std::vector<MacDiskDescription> disks;
    CFOwned<DASessionRef> session(DASessionCreate(kCFAllocatorDefault));
    if (!session) {
        return disks;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    // IOServiceGetMatchingServices consumes the matching dictionary.
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMedia"), &iterator) != KERN_SUCCESS) {
        return disks;
    }
    IOOwned ownedIterator(iterator);
    while (io_service_t service = IOIteratorNext(iterator)) {
        IOOwned media(service);
        CFOwned<DADiskRef> disk(DADiskCreateFromIOMedia(kCFAllocatorDefault, session.get(), service));
        if (!disk) {
            continue;
        }
        if (auto description = describe(disk.get())) {
            disks.push_back(std::move(*description));
        }
    }
    return disks;
}

std::optional<MacDiskDescription> describeDisk(const std::string &bsdName)
{
    CFOwned<DASessionRef> session(DASessionCreate(kCFAllocatorDefault));
    if (!session) {
        return std::nullopt;
    }
    CFOwned<DADiskRef> disk(DADiskCreateFromBSDName(kCFAllocatorDefault, session.get(), bsdName.c_str()));
    if (!disk) {
        return std::nullopt;
    }
    return describe(disk.get());
}

}  // namespace seabass::infrastructure::media
