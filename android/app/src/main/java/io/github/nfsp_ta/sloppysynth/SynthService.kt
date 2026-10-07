/* sloppy-synth: Android front end for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
package io.github.nfsp_ta.sloppysynth

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Keeps the synth playing while the app is in the background or the screen
 * is off, so a phone can sit on a desk as a MIDI sound module. Also owns the
 * MIDI inputs. The notification's Stop button shuts everything down.
 */
class SynthService : Service() {
    private var midi: MidiInputs? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutDown()
            return START_NOT_STICKY
        }

        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= 29)
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        else
            startForeground(NOTIFICATION_ID, notification)

        if (midi == null) midi = MidiInputs(this).also { it.start() }
        running = true
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        midi?.stop()
        midi = null
        running = false
        super.onDestroy()
    }

    private fun shutDown() {
        midi?.stop()
        midi = null
        NativeSynth.stopAudio()
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
        stopSelf()
        running = false
        onStopped?.invoke()
    }

    private fun buildNotification(): Notification {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26) {
            val channel = NotificationChannel(CHANNEL_ID, getString(R.string.channel_name),
                NotificationManager.IMPORTANCE_LOW)
            channel.setShowBadge(false)
            manager.createNotificationChannel(channel)
        }

        val immutable = if (Build.VERSION.SDK_INT >= 23) PendingIntent.FLAG_IMMUTABLE else 0
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), immutable)
        val stop = PendingIntent.getService(this, 1,
            Intent(this, SynthService::class.java).setAction(ACTION_STOP), immutable)

        val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL_ID)
                      else @Suppress("DEPRECATION") Notification.Builder(this)
        return builder
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(getString(R.string.notification_text))
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.stop), stop).build())
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "playing"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "io.github.nfsp_ta.sloppysynth.STOP"

        @Volatile
        var running = false
            private set

        /** Called on the main thread when the user stops the synth. */
        var onStopped: (() -> Unit)? = null

        fun start(context: Context) {
            val intent = Intent(context, SynthService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent)
            else context.startService(intent)
        }
    }
}
