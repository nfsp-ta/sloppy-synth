/* sloppy-synth: Android front end for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
package io.github.nfsp_ta.sloppysynth

import android.content.Context
import android.content.pm.PackageManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * Opens every MIDI device that sends notes (USB keyboards, other apps'
 * virtual ports), including ones plugged in later, and feeds them to the
 * engine. Program changes switch patches, as on the Linux player.
 */
class MidiInputs(private val context: Context) {
    private val manager: MidiManager? =
        if (context.packageManager.hasSystemFeature(PackageManager.FEATURE_MIDI))
            context.getSystemService(Context.MIDI_SERVICE) as MidiManager?
        else null

    private val handler = Handler(Looper.getMainLooper())
    private val devices = mutableMapOf<Int, MidiDevice>()
    private val ports = mutableMapOf<Int, List<MidiOutputPort>>()

    private val callback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(device: MidiDeviceInfo) = open(device)
        override fun onDeviceRemoved(device: MidiDeviceInfo) = close(device.id)
    }

    val available get() = manager != null

    fun start() {
        val manager = manager ?: return
        manager.registerDeviceCallback(callback, handler)
        @Suppress("DEPRECATION")
        for (device in manager.devices) open(device)
    }

    fun stop() {
        manager?.unregisterDeviceCallback(callback)
        for (id in devices.keys.toList()) close(id)
    }

    fun deviceNames(): List<String> = devices.values.map { name(it.info) }

    private fun open(info: MidiDeviceInfo) {
        // A device's "output" ports are the ones it sends MIDI out of.
        if (info.outputPortCount == 0 || devices.containsKey(info.id)) return
        manager?.openDevice(info, { device ->
            if (device == null) {
                Log.w(TAG, "Couldn't open MIDI device ${name(info)}")
                return@openDevice
            }
            devices[info.id] = device
            ports[info.id] = (0 until info.outputPortCount).mapNotNull { index ->
                device.openOutputPort(index)?.also { port ->
                    port.connect(Receiver(info.id * 256 + index))
                }
            }
            Log.i(TAG, "MIDI in: ${name(info)}")
        }, handler)
    }

    private fun close(id: Int) {
        ports.remove(id)?.forEach { it.close() }
        devices.remove(id)?.close()
    }

    private class Receiver(private val key: Int) : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            NativeSynth.midi(key, msg, offset, count)
        }
    }

    companion object {
        private const val TAG = "sloppy-synth"

        fun name(info: MidiDeviceInfo): String =
            info.properties.getString(MidiDeviceInfo.PROPERTY_NAME)
                ?: info.properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT)
                ?: "MIDI device ${info.id}"
    }
}
