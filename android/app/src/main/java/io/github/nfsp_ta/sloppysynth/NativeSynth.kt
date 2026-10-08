/* sloppy-synth: Android front end for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
package io.github.nfsp_ta.sloppysynth

import android.content.Context
import android.media.AudioManager
import android.util.Log
import java.io.File

/**
 * The engine, audio output and web UI server, all in native code. There is
 * one synth per process; it outlives the activity so it can keep playing
 * from MIDI with the screen off.
 */
object NativeSynth {
    private const val TAG = "sloppy-synth"

    init {
        System.loadLibrary("sloppy_android")
    }

    /** Port the web UI is served on, once [start] has succeeded. */
    @Volatile
    var port = -1
        private set

    /** Last startup problem, or null. */
    @Volatile
    var error: String? = null
        private set

    fun libraryDir(context: Context): File =
        File(context.getExternalFilesDir(null) ?: context.filesDir, "library")

    /**
     * Creates the synth, starts the web UI server and opens audio. Safe to
     * call more than once; later calls restart audio if it was stopped. Slow on old phones, so not on the main thread.
     * Returns the web UI port, or -1 with [error] set.
     */
    @Synchronized
    fun start(context: Context): Int {
        if (port > 0) {
            // Audio may have been stopped from the notification.
            nativeStartAudio().let { if (it.isNotEmpty()) Log.e(TAG, it) }
            return port
        }
        val app = context.applicationContext

        val webDir = WebAssets.install(app)
        val audio = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val rate = audio.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 0
        val burst = audio.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)?.toIntOrNull() ?: 0

        val createError = nativeCreate(app, libraryDir(app).absolutePath, webDir.absolutePath, rate, burst)
        if (createError.isNotEmpty()) {
            error = createError
            return -1
        }

        val audioError = nativeStartAudio()
        if (audioError.isNotEmpty()) Log.e(TAG, audioError)

        val serverPort = nativeStartServer(false)
        if (serverPort < 0) {
            error = "Couldn't start the web UI. Another app may be using ports 8080 to 8089."
            return -1
        }
        error = if (audioError.isNotEmpty()) audioError else null
        port = serverPort
        Log.i(TAG, "Started: ${nativeDescribeAudio()}, web UI on port $port")
        return port
    }

    fun startAudio(): String = nativeStartAudio()
    fun stopAudio() = nativeStopAudio()
    fun describeAudio(): String = nativeDescribeAudio()
    fun allNotesOff() = nativeAllNotesOff()

    /** Imports a .vital or .vitalbank file. Returns (worked, message). */
    fun import(file: File, displayName: String): Pair<Boolean, String> {
        val result = nativeImport(file.absolutePath, displayName)
        return if (result.startsWith("ok:")) true to result.removePrefix("ok:") else false to result
    }

    fun midi(port: Int, data: ByteArray, offset: Int, count: Int) = nativeMidi(port, data, offset, count)

    private external fun nativeCreate(
        context: Context, libraryDir: String, webDir: String,
        defaultSampleRate: Int, defaultFramesPerBurst: Int,
    ): String
    private external fun nativeStartServer(allowNetwork: Boolean): Int
    private external fun nativeStartAudio(): String
    private external fun nativeStopAudio()
    private external fun nativeDescribeAudio(): String
    private external fun nativeMidi(port: Int, data: ByteArray, offset: Int, count: Int)
    private external fun nativeAllNotesOff()
    private external fun nativeImport(path: String, displayName: String): String
}
