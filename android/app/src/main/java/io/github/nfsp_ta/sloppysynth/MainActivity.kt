/* sloppy-synth: Android front end for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
package io.github.nfsp_ta.sloppysynth

import android.Manifest
import android.annotation.SuppressLint
import android.annotation.TargetApi
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.util.Log
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.RenderProcessGoneDetail
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/**
 * Shows the web UI, served by the synth on 127.0.0.1, full screen. Also
 * where patches and banks come in: from the UI's Import button, or shared
 * or opened from another app (a file manager, a browser download).
 */
class MainActivity : Activity() {
    private lateinit var webView: WebView
    private lateinit var root: FrameLayout
    private val worker = Executors.newSingleThreadExecutor()
    private var port = -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // An instrument shouldn't go dark mid-song.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        webView = createWebView()
        root = FrameLayout(this).apply {
            setBackgroundColor(BACKGROUND)
            addView(webView)
        }
        layOutEdgeToEdge(root)
        setContentView(root)

        showMessage(getString(R.string.starting))
        SynthService.onStopped = { finishAndRemoveTask() }
        requestNotificationPermission()
        startSynth()
        handleIncoming(intent)
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun createWebView() = WebView(this).apply {
        setBackgroundColor(BACKGROUND)
        settings.javaScriptEnabled = true
        settings.domStorageEnabled = true
        settings.setSupportZoom(false)
        settings.builtInZoomControls = false
        // The UI's layout is designed for the screen size; system font
        // scaling would push controls off it.
        settings.textZoom = 100
        webViewClient = LocalOnlyClient()
        addJavascriptInterface(Bridge(), "SloppyAndroid")
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIncoming(intent)
    }

    override fun onDestroy() {
        SynthService.onStopped = null
        webView.destroy()
        worker.shutdown()
        super.onDestroy()
    }

    // Back leaves the synth playing, like a music player.
    @Deprecated("Still the simplest way to handle back on every Android version")
    override fun onBackPressed() {
        moveTaskToBack(true)
    }

    private fun startSynth() {
        worker.execute {
            val started = NativeSynth.start(this)
            runOnUiThread {
                if (isFinishing) return@runOnUiThread
                if (started < 0) {
                    showMessage(NativeSynth.error ?: getString(R.string.start_failed))
                    return@runOnUiThread
                }
                port = started
                SynthService.start(this)
                webView.loadUrl("http://127.0.0.1:$port/")
                NativeSynth.error?.let { toast(it) }
            }
        }
    }

    private fun showMessage(text: String) {
        val html = "<html><body style=\"background:#14161b;color:#e8eaef;font:16px system-ui,sans-serif;" +
            "display:flex;align-items:center;justify-content:center;height:90vh;margin:0;padding:24px;" +
            "text-align:center\">${android.text.Html.escapeHtml(text)}</body></html>"
        webView.loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
    }

    private fun toast(text: String) {
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_LONG).show()
    }

    // Draws behind the system bars on every Android version (Android 15
    // insists on it) and pads the UI by the bars' size, so the look is the
    // same everywhere.
    private fun layOutEdgeToEdge(root: View) {
        window.statusBarColor = BACKGROUND
        window.navigationBarColor = BACKGROUND
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
        } else {
            @Suppress("DEPRECATION")
            root.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
        root.setOnApplyWindowInsetsListener { view, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                view.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                    insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
    }

    private fun requestNotificationPermission() {
        // Without it the synth still plays in the background, but there's
        // no notification to stop it from.
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 0)
        }
    }

    // --- Importing patches and banks ---

    private fun handleIncoming(intent: Intent?) {
        val uri: Uri? = when (intent?.action) {
            Intent.ACTION_VIEW -> intent.data
            Intent.ACTION_SEND ->
                if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM)
            else -> null
        }
        if (uri != null) {
            // Handled once; rotating or reopening shouldn't import it again.
            intent?.action = null
            import(uri)
        }
    }

    private fun pickFile() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
        startActivityForResult(intent, REQUEST_IMPORT)
    }

    @Deprecated("Activity result API needs AndroidX; this works on every version")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == REQUEST_IMPORT && resultCode == RESULT_OK && uri != null) import(uri)
    }

    private fun import(uri: Uri) {
        worker.execute {
            val name = displayName(uri)
            val copy = File(cacheDir, "import-${System.nanoTime()}")
            val result = try {
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Couldn't open $name" }
                    copy.outputStream().use { input.copyTo(it) }
                }
                // Wait for the synth if the app was opened with a file.
                if (NativeSynth.start(this) < 0) false to (NativeSynth.error ?: getString(R.string.start_failed))
                else NativeSynth.import(copy, name)
            } catch (e: Exception) {
                false to (e.message ?: "Import failed")
            } finally {
                copy.delete()
            }
            runOnUiThread { reportImport(result.first, result.second) }
        }
    }

    private fun reportImport(ok: Boolean, message: String) {
        if (port < 0) {
            toast(message)
            return
        }
        val js = "window.sloppyImported ? window.sloppyImported(${JSONObject.quote(message)}, $ok) : 0"
        webView.evaluateJavascript(js) { result ->
            if (result == "0") toast(message)
        }
    }

    private fun displayName(uri: Uri): String {
        if (uri.scheme == "content") {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0)?.let { return it }
            }
        }
        return uri.lastPathSegment ?: "file"
    }

    /** What the web UI can ask of the app, as window.SloppyAndroid. */
    private inner class Bridge {
        @JavascriptInterface
        fun importFile() = runOnUiThread { pickFile() }

        @JavascriptInterface
        fun audioInfo(): String = NativeSynth.describeAudio()
    }

    /** Keeps the WebView on the synth's own pages; anything else opens in a browser. */
    private inner class LocalOnlyClient : WebViewClient() {
        // The page runs in a separate renderer process, which Android may
        // kill when memory runs short (an old phone with the synth loaded,
        // rotating the screen). Unhandled, that kills the whole app, synth
        // included; instead, make a new WebView and load the UI again.
        // Only called on Android 8+, where the renderer is a separate process.
        @TargetApi(26)
        override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
            Log.e(TAG, "Web UI renderer gone (crashed: ${detail.didCrash()}), reloading it")
            if (view !== webView) return true
            root.removeView(view)
            view.destroy()
            webView = createWebView()
            root.addView(webView)
            if (port > 0) webView.loadUrl("http://127.0.0.1:$port/")
            return true
        }

        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            val url = request.url
            if (url.host == "127.0.0.1" && url.port == port) return false
            try {
                startActivity(Intent(Intent.ACTION_VIEW, url))
            } catch (_: Exception) {
            }
            return true
        }
    }

    companion object {
        private const val TAG = "sloppy-synth"
        private const val REQUEST_IMPORT = 1
        private val BACKGROUND = Color.parseColor("#14161b")
    }
}
