import java.io.File
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.jetbrains.kotlin.android)
    alias(libs.plugins.meta.spatial.plugin)
    alias(libs.plugins.jetbrains.kotlin.plugin.compose)
}

// ---- Release signing -------------------------------------------------------------------------------------------
// The keystore and its passwords never live in the repository. They are read from a properties file:
//   1. the path in the environment variable QUESTVISOR_KEYSTORE_PROPERTIES, or
//   2. keystore.properties in the Gradle project root (git-ignored).
// Keys: storeFile (path, relative paths resolve against the properties file's folder), storePassword, keyAlias,
// keyPassword. Without it, debug builds work as before and any release task stops with an explanation.
class ReleaseKeystore(
    val storeFile: File,
    val storePassword: String,
    val keyAlias: String,
    val keyPassword: String,
)

val keystorePropertiesFile: File = System.getenv("QUESTVISOR_KEYSTORE_PROPERTIES")
    ?.takeIf { it.isNotBlank() }
    ?.let { File(it) }
    ?: rootProject.file("keystore.properties")

var releaseKeystoreProblem: String? = null
val releaseKeystore: ReleaseKeystore? = run {
    if (!keystorePropertiesFile.isFile) {
        releaseKeystoreProblem = "no signing properties file at ${keystorePropertiesFile.absolutePath}"
        return@run null
    }
    val props = Properties()
    keystorePropertiesFile.inputStream().use { props.load(it) }
    val missing = listOf("storeFile", "storePassword", "keyAlias", "keyPassword")
        .filter { props.getProperty(it).isNullOrBlank() }
    if (missing.isNotEmpty()) {
        releaseKeystoreProblem = "${keystorePropertiesFile.absolutePath} is missing: ${missing.joinToString()}"
        return@run null
    }
    val store = File(props.getProperty("storeFile")).let {
        if (it.isAbsolute) it else File(keystorePropertiesFile.absoluteFile.parentFile, it.path)
    }
    if (!store.isFile) {
        releaseKeystoreProblem = "keystore not found: ${store.absolutePath}"
        return@run null
    }
    ReleaseKeystore(store, props.getProperty("storePassword"), props.getProperty("keyAlias"),
        props.getProperty("keyPassword"))
}

tasks.matching { it.name == "preReleaseBuild" }.configureEach {
    doFirst {
        if (releaseKeystore == null) {
            throw GradleException(
                "Release signing is not configured: $releaseKeystoreProblem.\n" +
                    "Create a keystore (keytool -genkeypair -v -keystore questvisor-release.jks -keyalg RSA " +
                    "-keysize 4096 -validity 10000 -alias questvisor) and a properties file with storeFile, " +
                    "storePassword, keyAlias and keyPassword, either as keystore.properties next to " +
                    "settings.gradle.kts (git-ignored) or anywhere via QUESTVISOR_KEYSTORE_PROPERTIES. " +
                    "See keystore.properties.example. Debug builds (assembleDebug) need none of this."
            )
        }
    }
}

tasks.configureEach {
    // Do not disable strip*DebugSymbols: packaging reads its output, so a disabled strip task ships stale or no
    // native libraries (seen 2026-11-19: APK had an old libquestvisor.so, then none at all).

    val lower = name.lowercase()
    if (
        lower.contains("ovrmetrics") ||
        lower.contains("usagedata") ||
        lower.contains("collectusage") ||
        lower.startsWith("git")
    ) {
        enabled = false
    }
}

android {
    namespace = "com.xerx.questvisor"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.xerx.questvisor"
        minSdk = 34
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters.add("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags.add("-std=c++17")
                arguments.add("-DANDROID_STL=c++_shared")

                arguments.add("-DANDROID_ALLOW_UNDEFINED_SYMBOLS=ON")
                arguments.add("-DCMAKE_C_COMPILER_LAUNCHER=")
                arguments.add("-DCMAKE_CXX_COMPILER_LAUNCHER=")
                arguments.add("-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF")
            }
        }
    }

    packaging {
        resources.excludes.add("META-INF/LICENSE")
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }

    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = releaseKeystore.storeFile
                storePassword = releaseKeystore.storePassword
                keyAlias = releaseKeystore.keyAlias
                keyPassword = releaseKeystore.keyPassword
            }
        }
    }

    buildTypes {
        release {
            // Minify stays off for now. A trial R8 build succeeds and keeps NativePlayer and its JNI methods
            // (proguard-rules.pro), but PlayerActivity also reaches Spatial SDK members by reflection
            // (findNoArgMethod / readFieldValue), and a minified build has not been tested on the headset.
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation("androidx.appcompat:appcompat:1.7.0")

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)

    implementation(platform(libs.androidx.compose.bom))

    implementation(libs.meta.spatial.sdk.base)
    implementation(libs.meta.spatial.sdk.compose)
    implementation(libs.meta.spatial.sdk.toolkit)
    implementation(libs.meta.spatial.sdk.uiset)
    implementation(libs.meta.spatial.sdk.vr)

    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.runtime:runtime")

    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.kotlinx.coroutines.android)

    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.ui.test.junit4)
    debugImplementation(libs.androidx.ui.tooling)
    debugImplementation(libs.androidx.ui.test.manifest)
}

spatial {
    allowUsageDataCollection.set(false)
    // Custom shaders (soft depth occlusion for the video panel), compiled by the Spatial plugin.
    shaders {
        sources.add(project.layout.projectDirectory.dir("src/shaders"))
    }
}
