package com.vi.phoneclient

import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * PC_Server (vi) HTTP 客户端。
 * 协议：GET /ping；POST /transcribe (WAV)、/text (UTF-8)、/clear；鉴权 Bearer token。
 */
class ServerClient(private val ip: String, private val port: Int, private val token: String) {

    interface Callback {
        fun onOk(body: JSONObject)
        fun onErr(msg: String)
    }

    private val base: String get() = "http://$ip:$port"

    fun ping(cb: Callback) = get("/ping", cb)
    fun clear(cb: Callback) = post("/clear", ByteArray(0), "application/octet-stream", cb)
    fun sendText(text: String, cb: Callback) =
        post("/text", text.toByteArray(Charsets.UTF_8), "text/plain; charset=utf-8", cb)

    fun transcribe(wav: ByteArray, cb: Callback) =
        post("/transcribe", wav, "audio/wav", cb)

    private fun get(path: String, cb: Callback) {
        Thread {
            try {
                val conn = open(path)
                conn.requestMethod = "GET"
                finish(conn, cb)
            } catch (e: Exception) {
                cb.onErr(e.message ?: "连接失败")
            }
        }.start()
    }

    private fun post(path: String, body: ByteArray, contentType: String, cb: Callback) {
        Thread {
            try {
                val conn = open(path)
                conn.requestMethod = "POST"
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", contentType)
                if (token.isNotEmpty()) {
                    conn.setRequestProperty("Authorization", "Bearer $token")
                }
                conn.setFixedLengthStreamingMode(body.size)
                conn.outputStream.use { it.write(body) }
                finish(conn, cb)
            } catch (e: Exception) {
                cb.onErr(e.message ?: "连接失败")
            }
        }.start()
    }

    private fun open(path: String): HttpURLConnection {
        val conn = URL("$base$path").openConnection() as HttpURLConnection
        conn.connectTimeout = 4000
        conn.readTimeout = 60000  // 转写可能要几秒
        // 默认 keep-alive：复用 TCP 连接，省掉每次重新握手的开销
        return conn
    }

    private fun finish(conn: HttpURLConnection, cb: Callback) {
        val code = conn.responseCode
        val stream: InputStream? = if (code in 200..299) conn.inputStream else conn.errorStream
        val text = stream?.use {
            val out = ByteArrayOutputStream()
            val buf = ByteArray(4096)
            while (true) {
                val n = it.read(buf)
                if (n <= 0) break
                out.write(buf, 0, n)
            }
            out.toString("UTF-8")
        } ?: ""
        // 注意：不调用 disconnect()——保留 keep-alive 连接复用

        try {
            val json = JSONObject(text.trim())
            if (json.optBoolean("ok", false)) {
                cb.onOk(json)
            } else {
                cb.onErr(
                    "HTTP $code 服务端返回失败: " +
                        json.optString("error", text.take(60))
                )
            }
        } catch (e: Exception) {
            val bytes = text.toByteArray(Charsets.UTF_8).size
            android.util.Log.e(
                "ViVoice",
                "parse fail code=$code bytes=$bytes ex=${e.javaClass.simpleName}:${e.message} body=$text"
            )
            cb.onErr(
                "解析失败[${e.javaClass.simpleName}] b=$bytes: ${text.take(40)}"
            )
        }
    }
}
