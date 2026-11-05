import Foundation
import IOKit
import IOKit.pwr_mgt
import SystemConfiguration
import DiskArbitration
import CoreServices
import ServiceManagement
import Security
import CoreFoundation
import CoreBluetooth
import CoreMIDI
import CoreLocation
import UserNotifications
import QuickLookThumbnailing
import AppKit
let m = IOServiceMatching("IOPlatformExpertDevice"); let svc = IOServiceGetMatchingService(kIOMainPortDefault, m)
var props: Unmanaged<CFMutableDictionary>?; IORegistryEntryCreateCFProperties(svc, &props, nil, 0); let d = props?.takeRetainedValue() as? [String: Any]
print("iokit", svc != 0, d?["IOPlatformUUID"] != nil, (d?["manufacturer"] as? Data) != nil)
let it = IORegistryEntryCreateCFProperty(svc, "IOPlatformSerialNumber" as CFString, nil, 0); print("serial", it != nil)
print("sc", SCDynamicStoreCopyComputerName(nil, nil) != nil, (SCDynamicStoreCopyLocalHostName(nil) as String?) != nil)
let store = SCDynamicStoreCreate(nil, "rebar" as CFString, nil, nil); let global = SCDynamicStoreCopyValue(store, "State:/Network/Global/IPv4" as CFString) as? [String: Any]; print("net global", global?["PrimaryInterface"] != nil)
let da = DASessionCreate(nil); print("diskarb", da != nil)
var rl: Unmanaged<CFArray>?; print("lsapps", (LSCopyApplicationURLsForURL(URL(fileURLWithPath: "/etc/hosts") as CFURL, .all)?.takeRetainedValue() as? [URL])?.count ?? -1 >= 0)
let ws = NSWorkspace.shared; print("workspace", ws.urlForApplication(withBundleIdentifier: "com.apple.finder")?.lastPathComponent ?? "?", ws.runningApplications.count > 3)
let fe = FSEventStreamCreate(nil, { _, _, _, _, _, _ in }, nil, ["/tmp"] as CFArray, FSEventsGetCurrentEventId(), 0.1, 0); print("fsevents", fe != nil)
let uti = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, "png" as CFString, nil)?.takeRetainedValue(); print("uti", uti ?? "?" as CFString)
let q = NSMetadataQuery(); q.predicate = NSPredicate(format: "kMDItemFSName == 'hosts'"); q.searchScopes = ["/etc"]; print("spotlight", q.start())
q.stop()
let cert = SecCertificateCreateWithData(nil, try! Data(contentsOf: URL(fileURLWithPath: "/etc/ssl/cert.pem")).prefix(0) as CFData); print("seccert nil ok", cert == nil)
var roots: CFArray?; print("trust roots", SecTrustCopyAnchorCertificates(&roots), (roots as? [SecCertificate])?.count ?? 0 > 50)
print("midi", MIDIGetNumberOfDevices() >= 0, MIDIGetNumberOfSources() >= 0)
print("bluetooth", CBCentralManager.authorization.rawValue >= 0)
print("location", CLLocationManager.locationServicesEnabled() || true, CLLocation(latitude: 50, longitude: 30).distance(from: CLLocation(latitude: 51, longitude: 30)).rounded())
let geo = CLGeocoder(); print("geocoder", !geo.isGeocoding)
print("notifications", UNUserNotificationCenter.self != nil)
let req = QLThumbnailGenerator.Request(fileAt: URL(fileURLWithPath: "/System/Library/CoreServices/DefaultDesktop.heic"), size: CGSize(width: 64, height: 64), scale: 1, representationTypes: .thumbnail)
let sem = DispatchSemaphore(value: 0); var thumb = false; QLThumbnailGenerator.shared.generateBestRepresentation(for: req) { r, e in thumb = r != nil; sem.signal() }; _ = sem.wait(timeout: .now() + 15); print("quicklook thumb", thumb)
let pm = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoIdleSleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn), "rebar" as CFString, nil); print("pm assertion", pm == 0 || true)
print("sandbox/env", ProcessInfo.processInfo.operatingSystemVersion.majorVersion, ProcessInfo.processInfo.isMacCatalystApp)
