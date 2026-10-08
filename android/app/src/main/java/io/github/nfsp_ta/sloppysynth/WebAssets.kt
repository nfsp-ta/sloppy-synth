/* sloppy-synth: Android front end for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
package io.github.nfsp_ta.sloppysynth

import android.content.Context
import java.io.File

/**
 * The control server serves the web UI from a folder, so the copy packed in
 * the APK's assets is unpacked into app storage, again after each update.
 */
object WebAssets {
    private const val ASSET_DIR = "web"

    fun install(context: Context): File {
        val target = File(context.filesDir, ASSET_DIR)
        val stamp = File(context.filesDir, "$ASSET_DIR.version")
        val version = installedVersion(context)

        if (!target.isDirectory || !stamp.isFile || stamp.readText() != version) {
            target.deleteRecursively()
            copy(context, ASSET_DIR, target)
            stamp.writeText(version)
        }
        return target
    }

    private fun installedVersion(context: Context): String {
        // lastUpdateTime changes on every install, so debug builds that
        // keep the same version code still refresh the UI.
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        @Suppress("DEPRECATION")
        val code = if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
        return "$code:${info.lastUpdateTime}"
    }

    private fun copy(context: Context, assetPath: String, target: File) {
        val children = context.assets.list(assetPath) ?: emptyArray()
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        target.mkdirs()
        for (child in children) copy(context, "$assetPath/$child", File(target, child))
    }
}
