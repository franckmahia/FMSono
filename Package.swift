// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "FMSono",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "FMSono", targets: ["FMSono"])],
    targets: [
        .target(name: "SonoDSP", publicHeadersPath: "include", linkerSettings: [
            .linkedFramework("CoreAudio"), .linkedFramework("AudioToolbox"), .linkedFramework("CoreFoundation")
        ]),
        .executableTarget(name: "FMSono", dependencies: ["SonoDSP"])
    ],
    swiftLanguageModes: [.v5]
)
