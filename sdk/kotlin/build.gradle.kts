plugins {
    kotlin("jvm") version "1.9.22"
}

group = "art.solun"
version = "0.1.0"

repositories {
    mavenCentral()
    google()
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0")
}

kotlin {
    jvmToolchain(17)
}
