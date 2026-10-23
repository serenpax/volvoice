package com.vi.phoneclient

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject

/**
 * Vi Voice — 手机语音遥控（遥控器布局）。
 * 中央圆形按钮按住说话 → 松手 → WAV 上传 PC_Server → 小米 ASR → 文字打进 PC 焦点窗口。
 * 外圈功能键：可编辑文本宏（长按编辑），内容为空 = 清空 PC 输入框。
 */
class MainActivity : Activity() {

    // ---- 状态 ----
    private lateinit var prefs: android.content.SharedPreferences
    private var client: ServerClient? = null
    private var connected = false

    // ---- 视图 ----
    private lateinit var root: LinearLayout
    private lateinit var dot: TextView
    private lateinit var serverLabel: TextView
    private lateinit var ptt: TextView
    private lateinit var result: TextView

    // ---- 录音 ----
    private val recorder = PttRecorder()

    // ---- 颜色（语义状态色，三主题通用） ----
    private val cGreen = Color.parseColor("#2E7D32")
    private val cRed = Color.parseColor("#C62828")
    private val cYellow = Color.parseColor("#F9A825")
    private val cGray = Color.parseColor("#9E9E9E")
    private val cSatBg = Color.parseColor("#E8F5E9")
    private val cSatFg = Color.parseColor("#1B5E20")

    // ---- 主题 ----
    private data class Theme(
        val bg: Int, val statusText: Int,
        val resultBg: Int, val resultText: Int,
        val pttIdle: Int, val satBg: Int, val satFg: Int,
        val dotOn: Int, val dotOff: Int
    )

    private fun theme(): Theme = when (prefs.getString("theme", "light")) {
        // 护眼：豆沙绿/米色低刺激
        "eye" -> Theme(
            bg = Color.parseColor("#E9E4D5"),
            statusText = Color.parseColor("#4A4238"),
            resultBg = Color.parseColor("#DDD6C4"),
            resultText = Color.parseColor("#3A342B"),
            pttIdle = Color.parseColor("#6F8F5A"),
            satBg = Color.parseColor("#D8DCC8"),
            satFg = Color.parseColor("#3E4A2E"),
            dotOn = Color.parseColor("#4E7A3E"),
            dotOff = Color.parseColor("#9A927F")
        )
        // 黑色
        "dark" -> Theme(
            bg = Color.parseColor("#121212"),
            statusText = Color.parseColor("#BDBDBD"),
            resultBg = Color.parseColor("#1E1E1E"),
            resultText = Color.parseColor("#E0E0E0"),
            pttIdle = Color.parseColor("#388E3C"),
            satBg = Color.parseColor("#263228"),
            satFg = Color.parseColor("#A5D6A7"),
            dotOn = Color.parseColor("#66BB6A"),
            dotOff = Color.parseColor("#757575")
        )
        // 亮色（默认）
        else -> Theme(
            bg = Color.parseColor("#FAFAFA"),
            statusText = Color.parseColor("#424242"),
            resultBg = Color.parseColor("#EEEEEE"),
            resultText = Color.parseColor("#212121"),
            pttIdle = cGreen,
            satBg = cSatBg,
            satFg = cSatFg,
            dotOn = cGreen,
            dotOff = cGray
        )
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("vi_cfg", MODE_PRIVATE)
        buildUi()
        ensureMicPermission()
    }

    override fun onResume() {
        super.onResume()
        rebuildClient()
        ping()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    // ================= UI =================

    private fun buildUi() {
        val t = theme()
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(24), dp(16), dp(12))
            setBackgroundColor(t.bg)
        }

        // targetSdk 35 强制沉浸式：手动给顶部加状态栏高度
        root.setOnApplyWindowInsetsListener { v, insets ->
            val top = insets.systemWindowInsetTop
            v.setPadding(dp(16), top + dp(8), dp(16), dp(12))
            insets
        }

        // 状态行
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        dot = TextView(this).apply {
            text = "●"
            textSize = 20f
            setTextColor(t.dotOff)
        }
        serverLabel = TextView(this).apply {
            textSize = 12f
            setTextColor(t.statusText)
            setPadding(dp(8), 0, dp(8), 0)
        }
        val settingsBtn = Button(this).apply {
            text = "设置"
            textSize = 12f
            minWidth = 0
            setPadding(dp(16), 0, dp(16), 0)
            setOnClickListener { showSettingsDialog() }
        }
        row.addView(dot)
        row.addView(serverLabel, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(settingsBtn)
        root.addView(row)

        // 径向主区（遥控器布局）
        val radial = RadialLayout(this)

        // 中央：按住说话
        ptt = TextView(this).apply {
            text = "按住\n说话"
            textSize = 19f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            background = circleDrawable(t.pttIdle)
            setOnTouchListener { v, ev ->
                when (ev.action) {
                    MotionEvent.ACTION_DOWN -> {
                        startRecording(); true
                    }
                    MotionEvent.ACTION_UP -> {
                        v.performClick(); stopAndSend(); true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        cancelRecording(); true
                    }
                    else -> false
                }
            }
        }
        radial.addView(ptt)

        // 外圈：功能键（可拨动旋转调整位置，点按触发）
        val buttons = loadButtons()
        for (i in 0 until 8) {
            val item = if (i < buttons.length()) buttons.getJSONObject(i)
            else JSONObject().put("label", "＋").put("payload", "")
            radial.addView(makeSatellite(item))
        }
        root.addView(radial, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        // 结果区
        result = TextView(this).apply {
            textSize = 13f
            setTextColor(t.resultText)
            setPadding(dp(12), dp(12), dp(12), dp(12))
            setBackgroundColor(t.resultBg)
            maxLines = 4
            text = "就绪。按住中央按钮说话，点外圈按键发预设指令（长按可编辑）。"
        }
        root.addView(result, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))

        setContentView(root)
    }

    private fun makeSatellite(item: JSONObject): TextView {
        val label = item.optString("label", "＋")
        val payload = item.optString("payload", "")
        val t = theme()
        return TextView(this).apply {
            text = label
            textSize = 13f
            setTextColor(t.satFg)
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(t.satBg)
            }
            setOnClickListener {
                if (payload.isEmpty() && label == "＋") editButton(label, payload)
                else fireButton(label, payload)
            }
            setOnLongClickListener {
                editButton(label, payload)
                true
            }
        }
    }

    private fun circleDrawable(color: Int): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(color)
        }

    private fun setPttColor(color: Int) {
        ptt.background = circleDrawable(color)
    }

    private fun rebuildRadial() {
        // 重建整个界面最简单可靠
        buildUi()
    }

    // ================= 功能键 =================

    private fun fireButton(label: String, payload: String) {
        val c = client ?: return showResult("未配置服务器，请点右上角设置")
        showResult("[$label] 发送中…")
        if (payload.isEmpty()) {
            c.clear(object : ServerClient.Callback {
                override fun onOk(body: JSONObject) = showResult("[$label] 已清空 PC 输入框")
                override fun onErr(msg: String) = showResult("[$label] 失败：$msg")
            })
        } else {
            c.sendText(payload, object : ServerClient.Callback {
                override fun onOk(body: JSONObject) = showResult("[$label] 已发送：$payload")
                override fun onErr(msg: String) = showResult("[$label] 失败：$msg")
            })
        }
    }

    private fun editButton(currentLabel: String, currentPayload: String) {
        // 找到该按钮的槽位
        val buttons = loadButtons()
        var index = -1
        for (i in 0 until buttons.length()) {
            val o = buttons.getJSONObject(i)
            if (o.optString("label") == currentLabel &&
                o.optString("payload") == currentPayload
            ) {
                index = i
                break
            }
        }
        if (index < 0) index = 0

        val ctx = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        val labelEdit = EditText(this).apply {
            hint = "按键名称（如：继续）"
            setText(currentLabel)
        }
        val payloadEdit = EditText(this).apply {
            hint = "发送的文字（留空 = 清空输入框）"
            setText(currentPayload)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 2
        }
        ctx.addView(labelEdit)
        ctx.addView(payloadEdit)

        AlertDialog.Builder(this)
            .setTitle("编辑功能键 ${index + 1}")
            .setView(ctx)
            .setPositiveButton("保存") { _, _ ->
                saveButton(index, JSONObject()
                    .put("label", labelEdit.text.toString().ifEmpty { "＋" })
                    .put("payload", payloadEdit.text.toString()))
                rebuildRadial()
            }
            .setNegativeButton("取消", null)
            .setNeutralButton("清空此键") { _, _ ->
                saveButton(index, JSONObject().put("label", "＋").put("payload", ""))
                rebuildRadial()
            }
            .show()
    }

    // ================= 录音与转写 =================

    private fun startRecording() {
        if (!recorder.start()) {
            showResult("录音启动失败：请检查麦克风权限")
            return
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setPttColor(cRed)
        ptt.text = "松手\n发送"
        showResult("录音中…")
    }

    private fun cancelRecording() {
        recorder.cancel()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setPttColor(theme().pttIdle)
        ptt.text = "按住\n说话"
        showResult("已取消")
    }

    private fun stopAndSend() {
        val wav = recorder.stop()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setPttColor(cYellow)
        ptt.text = "识别中…"
        if (wav.isEmpty()) {
            setPttColor(theme().pttIdle); ptt.text = "按住\n说话"
            showResult("没有录到声音")
            return
        }
        showResult("识别中…（约 ${wav.size / 32000} 秒音频）")
        val c = client ?: return run {
            setPttColor(theme().pttIdle); ptt.text = "按住\n说话"
            showResult("未配置服务器，请点右上角设置")
        }
        c.transcribe(wav, object : ServerClient.Callback {
            override fun onOk(body: JSONObject) = runOnUiThread {
                setPttColor(theme().pttIdle)
                ptt.text = "按住\n说话"
                val text = body.optString("text", "")
                val asr = body.optLong("asr_ms", -1)
                val paste = body.optLong("paste_ms", -1)
                val total = body.optLong("elapsed_ms", -1)
                showResult("已输入（转写 ${asr}ms / 上屏 ${paste}ms / 总 ${total}ms）：\n$text")
            }
            override fun onErr(msg: String) = runOnUiThread {
                setPttColor(theme().pttIdle)
                ptt.text = "按住\n说话"
                showResult("失败：$msg")
            }
        })
    }

    // ================= 连接 =================

    private fun rebuildClient() {
        val ip = prefs.getString("ip", "") ?: ""
        val port = prefs.getInt("port", 30686)
        val token = prefs.getString("token", "") ?: ""
        client = if (ip.isEmpty()) null else ServerClient(ip, port, token)
    }

    private fun ping() {
        val c = client
        if (c == null) {
            connected = false
            updateStatus()
            return
        }
        c.ping(object : ServerClient.Callback {
            override fun onOk(body: JSONObject) {
                connected = true
                updateStatus()
            }
            override fun onErr(msg: String) {
                connected = false
                updateStatus()
            }
        })
    }

    private fun updateStatus() = runOnUiThread {
        val t = theme()
        val ip = prefs.getString("ip", "") ?: ""
        val port = prefs.getInt("port", 30686)
        if (connected) {
            dot.setTextColor(t.dotOn)
            serverLabel.text = "已连接  http://$ip:$port"
        } else {
            dot.setTextColor(t.dotOff)
            serverLabel.text = if (ip.isEmpty()) "未配置服务器" else "未连接  http://$ip:$port"
        }
    }

    private fun showSettingsDialog() {
        val ctx = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        val ipEdit = EditText(this).apply {
            hint = "PC 局域网 IP（如 192.168.1.10）"
            setText(prefs.getString("ip", "") ?: "")
        }
        val portEdit = EditText(this).apply {
            hint = "端口"
            inputType = InputType.TYPE_CLASS_NUMBER
            setText(prefs.getInt("port", 30686).toString())
        }
        val tokenEdit = EditText(this).apply {
            hint = "token（与 PC 的 config.json 一致）"
            setText(prefs.getString("token", "") ?: "")
        }
        // 主题选择：亮色 / 护眼 / 黑色
        val themeGroup = android.widget.RadioGroup(this).apply {
            orientation = android.widget.RadioGroup.VERTICAL
        }
        val rbLight = android.widget.RadioButton(this).apply {
            text = "亮色"
            id = View.generateViewId()
        }
        val rbEye = android.widget.RadioButton(this).apply {
            text = "护眼"
            id = View.generateViewId()
        }
        val rbDark = android.widget.RadioButton(this).apply {
            text = "黑色"
            id = View.generateViewId()
        }
        themeGroup.addView(rbLight)
        themeGroup.addView(rbEye)
        themeGroup.addView(rbDark)
        when (prefs.getString("theme", "light")) {
            "eye" -> rbEye.isChecked = true
            "dark" -> rbDark.isChecked = true
            else -> rbLight.isChecked = true
        }
        ctx.addView(ipEdit)
        ctx.addView(portEdit)
        ctx.addView(tokenEdit)
        ctx.addView(themeGroup)

        AlertDialog.Builder(this)
            .setTitle("连接设置")
            .setView(ctx)
            .setPositiveButton("保存") { _, _ ->
                val themeVal = when (themeGroup.checkedRadioButtonId) {
                    rbEye.id -> "eye"
                    rbDark.id -> "dark"
                    else -> "light"
                }
                prefs.edit()
                    .putString("ip", ipEdit.text.toString().trim())
                    .putInt("port", portEdit.text.toString().toIntOrNull() ?: 30686)
                    .putString("token", tokenEdit.text.toString().trim())
                    .putString("theme", themeVal)
                    .apply()
                rebuildRadial()  // 主题变化需重建界面
                rebuildClient()
                ping()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    // ================= 按键持久化 =================

    private fun loadButtons(): JSONArray {
        return try {
            JSONArray(prefs.getString("buttons", defaultButtons().toString())!!)
        } catch (e: Exception) {
            defaultButtons()
        }
    }

    private fun saveButton(index: Int, obj: JSONObject) {
        val arr = loadButtons()
        while (arr.length() <= index) arr.put(JSONObject().put("label", "＋").put("payload", ""))
        arr.put(index, obj)
        prefs.edit().putString("buttons", arr.toString()).apply()
    }

    private fun defaultButtons(): JSONArray = JSONArray()
        .put(JSONObject().put("label", "继续").put("payload", "继续"))
        .put(JSONObject().put("label", "解释").put("payload", "解释这段代码"))
        .put(JSONObject().put("label", "测试").put("payload", "帮我补上单元测试"))
        .put(JSONObject().put("label", "修复").put("payload", "帮我修复这个问题"))
        .put(JSONObject().put("label", "重构").put("payload", "重构这段代码，保持行为不变"))
        .put(JSONObject().put("label", "提交").put("payload", "总结本次改动并写 commit message"))
        .put(JSONObject().put("label", "清空").put("payload", ""))
        .put(JSONObject().put("label", "＋").put("payload", ""))

    // ================= 杂项 =================

    private fun showResult(msg: String) = runOnUiThread {
        result.text = msg
    }

    private fun ensureMicPermission() {
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), 100)
        }
    }
}
