plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.github.nfsp_ta.sloppysynth"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "io.github.nfsp_ta.sloppysynth"
        // Android 6: old phones, and the first version with the MIDI API.
        minSdk = 23
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // Vital's DSP needs NEON or SSE2. 32-bit ARM covers the oldest
            // phones; x86_64 is for the emulator and Chromebooks.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // The engine is far too slow unoptimised to play in real
                // time, so even debug builds get an optimised engine.
                arguments += listOf("-DCMAKE_BUILD_TYPE=Release", "-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key until there's a release key, so
            // release builds can be installed for testing.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // Uncompressed native libraries load straight from the APK.
            useLegacyPackaging = false
        }
    }
}

// The web UI lives in the repository's web/ folder, shared with the Linux
// player. It's copied into the APK's assets at build time.
val webUiAssets = layout.buildDirectory.dir("generated/webui")
val copyWebUi by tasks.registering(Copy::class) {
    from(rootProject.file("../web"))
    into(webUiAssets.map { it.dir("web") })
}
android.sourceSets["main"].assets.srcDir(webUiAssets)
tasks.named("preBuild") { dependsOn(copyWebUi) }

dependencies {
    implementation("com.google.oboe:oboe:1.9.3")
}
