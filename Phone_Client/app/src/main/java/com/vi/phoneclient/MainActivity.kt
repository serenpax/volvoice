package com.vi.phoneclient

import android.app.Activity
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject

/**
 * Vi Voice — 方案 E「精准暗调 × 对讲机」。
 * 中央 PTT 按住说话 → 松手 → WAV 上传服务端 → ASR → 文字上屏。
 * 外圈 8 个频道键（长按编辑）；顶部 RX/TX 指示；底部信号格 + 扬声器格栅。
 */
class MainActivity : Activity() {

    // ---- 调色板（方案 E 固定主题） ----
    private val C_BG = 0xFF0C0C11.toInt()
    private val C_PANEL = 0xFF101018.toInt()
    private val C_CHIP = 0xFF14141C.toInt()
    private val C_INK = 0xFFECECF0.toInt()
    private val C_INK_DIM = 0xFF9A9DAB.toInt()
    private val C_INK_FAINT = 0xFF5B5E6B.toInt()
    private val C_ACCENT = 0xFF7C8CFF.toInt()
    private val C_RX = 0xFF4ADE80.toInt()
    private val C_TX = 0xFFFF5A5F.toInt()
    private val C_HAIR = 0x14FFFFFF.toInt()
    private val C_HAIR_SOFT = 0x0DFFFFFF.toInt()

    // ---- 状态 ----
    private lateinit var prefs: android.content.SharedPreferences
    private var client: ServerClient? = null
    private var connected = false

    // ---- 多服务器档案 ----
    private data class ServerProfile(
        var name: String, var ip: String, var port: Int, var token: String
    )
    private var servers = mutableListOf<ServerProfile>()
    private var activeIdx = 0

    // ---- 视图 ----
    private lateinit var root: FrameLayout
    private lateinit var chipLed: View
    private lateinit var chipLabel: TextView
    private lateinit var ptt: LinearLayout
    private lateinit var pttMic: ImageView
    private lateinit var pttLabel: TextView
    private lateinit var pttSub: TextView
    private lateinit var grille: GrilleView
    private lateinit var waveform: WaveformView
    private lateinit var signalBars: SignalBarsView
    private lateinit var macText: TextView
    private lateinit var msgText: TextView
    private lateinit var statAsr: TextView
    private lateinit var statPaste: TextView
    private lateinit var statTotal: TextView

    private var transmitting = false

    // ---- 录音 ----
    private val recorder = PttRecorder()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("vi_cfg", MODE_PRIVATE)
        servers = loadServers()
        activeIdx = prefs.getInt("active", 0)
            .coerceIn(0, (servers.size - 1).coerceAtLeast(0))
        buildUi()
        ensureMicPermission()
    }

    override fun onResume() {
        super.onResume()
        rebuildClient()
        ping()
    }

    private fun dp(v: Float): Int =
        (v * resources.displayMetrics.density + 0.5f).toInt()

    private fun dp(v: Int): Int = dp(v.toFloat())

    private fun mono(sizeSp: Float, color: Int, bold: Boolean = true): TextView =
        TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
            setTextColor(color)
            typeface = Typeface.create("monospace", if (bold) Typeface.BOLD else Typeface.NORMAL)
        }

    // ================= 多服务器档案 =================

    private fun loadServers(): MutableList<ServerProfile> {
        val out = mutableListOf<ServerProfile>()
        try {
            val arr = JSONArray(prefs.getString("servers", "[]")!!)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                out.add(
                    ServerProfile(
                        name = o.optString("name", "电脑${i + 1}"),
                        ip = o.optString("ip", ""),
                        port = o.optInt("port", 30686),
                        token = o.optString("token", "")
                    )
                )
            }
        } catch (_: Exception) {
        }
        if (out.isEmpty()) {
            val ip = prefs.getString("ip", "") ?: ""
            if (ip.isNotEmpty()) {
                out.add(
                    ServerProfile(
                        name = "我的电脑", ip = ip,
                        port = prefs.getInt("port", 30686),
                        token = prefs.getString("token", "") ?: ""
                    )
                )
            }
        }
        return out
    }

    private fun persistServers() {
        val arr = JSONArray()
        for (s in servers) {
            arr.put(
                JSONObject()
                    .put("name", s.name).put("ip", s.ip)
                    .put("port", s.port).put("token", s.token)
            )
        }
        prefs.edit()
            .putString("servers", arr.toString())
            .putInt("active", activeIdx)
            .apply()
    }

    private fun activeServer(): ServerProfile? = servers.getOrNull(activeIdx)

    // ================= UI 构建 =================

    private fun buildUi() {
        root = FrameLayout(this).apply { setBackgroundColor(C_BG) }

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24f), dp(8f), dp(24f), dp(16f))
        }
        root.setOnApplyWindowInsetsListener { v, insets ->
            content.setPadding(dp(24f), insets.systemWindowInsetTop + dp(8f), dp(24f), dp(16f))
            insets
        }

        // ---- 顶栏：品牌 + 频段牌 + RX/TX 章 ----
        content.addView(buildHeader())

        // ---- 转盘区 ----
        content.addView(buildWheel(), LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        // ---- 底部结果面板 ----
        content.addView(buildResultPanel(), LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))

        root.addView(content, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))
        setContentView(root)
        applyRxState()
        updateMacText()
        updateMsgIdle()
    }

    private fun buildHeader(): LinearLayout {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4f), dp(12f), dp(4f), dp(8f))
        }
        // 品牌方标
        row.addView(View(this).apply {
            background = getDrawable(R.drawable.brand_mark)
            layoutParams = LinearLayout.LayoutParams(dp(10f), dp(10f))
        })
        row.addView(TextView(this).apply {
            text = "Volvoice"
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setTextColor(C_INK)
            typeface = Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { marginStart = dp(9f) }
        })
        // 频段牌 2.4G · CH01
        row.addView(TextView(this).apply {
            text = "2.4G · CH01"
            typeface = Typeface.create("monospace", Typeface.BOLD)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f)
            setTextColor(0x4DFFFFFF.toInt())
            background = getDrawable(R.drawable.chip_pill)
            setPadding(dp(7f), dp(3f), dp(7f), dp(3f))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { marginStart = dp(8f) }
        })
        row.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))

        // RX / TX 指示章（点按切换电脑，长按编辑）
        val chip = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = getDrawable(R.drawable.chip_pill)
            setPadding(dp(13f), dp(7f), dp(13f), dp(7f))
            isClickable = true
            setOnClickListener { showServerDialog() }
            setOnLongClickListener { showSettingsDialog(); true }
        }
        chipLed = View(this).apply {
            background = circle(C_RX, 8f)
            layoutParams = LinearLayout.LayoutParams(dp(7f), dp(7f))
        }
        chipLabel = mono(11f, C_RX).apply {
            text = "RX"
            letterSpacing = 0.15f
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { marginStart = dp(7f) }
        }
        chip.addView(chipLed)
        chip.addView(chipLabel)
        row.addView(chip)
        return row
    }

    /** 转盘容器：发丝环 + 刻度环 + 刻度数字 + RadialLayout */
    private fun buildWheel(): FrameLayout {
        val box = FrameLayout(this)

        // 发丝外环
        box.addView(View(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(Color.TRANSPARENT)
                setStroke(dp(1f), C_HAIR_SOFT)
            }
        })

        // 频率刻度环
        box.addView(DialTicksView(this))

        // 径向布局：PTT + 8 频道键
        val radial = RadialLayout(this)

        // 中央 PTT
        ptt = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            background = getDrawable(R.drawable.ptt_idle)
            isClickable = true
            setOnTouchListener { v, ev ->
                when (ev.action) {
                    MotionEvent.ACTION_DOWN -> { startRecording(); true }
                    MotionEvent.ACTION_UP -> { v.performClick(); stopAndSend(); true }
                    MotionEvent.ACTION_CANCEL -> { cancelRecording(); true }
                    else -> false
                }
            }
        }
        pttMic = ImageView(this).apply {
            setImageResource(R.drawable.ic_mic)
            setColorFilter(C_ACCENT)
            layoutParams = LinearLayout.LayoutParams(dp(28f), dp(28f))
        }
        pttLabel = TextView(this).apply {
            text = "按住说话"
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextColor(0xFFC8CAD4.toInt())
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.2f
            gravity = Gravity.CENTER
            setPadding(0, dp(2f), 0, 0)
        }
        pttSub = mono(8f, 0x4DFFFFFF.toInt(), bold = false).apply {
            text = "PUSH TO TALK"
            letterSpacing = 0.25f
            gravity = Gravity.CENTER
            setPadding(0, dp(1f), 0, 0)
        }
        val pttWrap = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        )
        ptt.addView(pttMic)
        ptt.addView(pttLabel, pttWrap)
        ptt.addView(pttSub, pttWrap)
        radial.addView(ptt)

        // 外圈频道键
        val buttons = loadButtons()
        for (i in 0 until 8) {
            val item = if (i < buttons.length()) buttons.getJSONObject(i)
            else JSONObject().put("label", "＋").put("payload", "")
            radial.addView(makeSatellite(i + 1, item))
        }
        box.addView(radial)
        return box
    }

    private fun makeSatellite(ch: Int, item: JSONObject): LinearLayout {
        val label = item.optString("label", "＋")
        val payload = item.optString("payload", "")
        // 注意：竖向 LinearLayout 默认子 View 宽度是 MATCH_PARENT（文字会靠左），
        // 必须显式 WRAP_CONTENT + gravity=CENTER 才能真正居中。
        val wrap = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        )
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            background = getDrawable(R.drawable.sat_circle)
            isClickable = true
            addView(mono(8.5f, 0x47FFFFFF.toInt()).apply {
                text = String.format("%02d", ch)
                letterSpacing = 0.1f
                gravity = Gravity.CENTER
            }, wrap)
            addView(TextView(this@MainActivity).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                setTextColor(C_INK_DIM)
                typeface = Typeface.DEFAULT_BOLD
                gravity = Gravity.CENTER
                setPadding(0, dp(1f), 0, 0)
            }, wrap)
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

    private fun buildResultPanel(): LinearLayout {
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = getDrawable(R.drawable.card_bg)
            setPadding(dp(16f), dp(14f), dp(16f), dp(14f))
        }
        val lp = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { marginStart = dp(2f); marginEnd = dp(2f) }
        card.layoutParams = lp

        // 信号行
        val sigRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        sigRow.addView(mono(9f, C_INK_FAINT).apply { text = "SIGNAL"; letterSpacing = 0.15f })
        signalBars = SignalBarsView(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(28f), dp(12f)).apply {
                marginStart = dp(8f); marginEnd = dp(8f)
            }
        }
        sigRow.addView(signalBars)
        macText = mono(10f, 0xFF8A8D9B.toInt(), bold = false).apply {
            letterSpacing = 0.05f
        }
        sigRow.addView(macText, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(sigRow)

        // 扬声器格栅
        grille = GrilleView(this).apply {
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(14f)).apply {
                topMargin = dp(12f); bottomMargin = dp(12f)
            }
        }
        card.addView(grille)

        // 发射波形（默认隐藏）
        waveform = WaveformView(this).apply {
            visibility = View.GONE
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(22f))
                .apply { bottomMargin = dp(10f) }
        }
        card.addView(waveform)

        // 消息行
        msgText = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextColor(0xFFC8CAD4.toInt())
            setPadding(0, 0, 0, dp(10f))
        }
        card.addView(msgText)

        // 发丝分隔线 + 统计行
        val divider = View(this).apply {
            setBackgroundColor(C_HAIR_SOFT)
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1f))
                .apply { bottomMargin = dp(10f) }
        }
        card.addView(divider)
        val cells = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun addStat(key: String): TextView {
            val cell = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            cell.addView(mono(10f, C_INK_FAINT).apply {
                text = key; letterSpacing = 0.1f
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { marginEnd = dp(6f) }
            })
            val v = mono(12f, C_INK).apply { text = "—" }
            cell.addView(v)
            cells.addView(cell, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { marginEnd = dp(18f) })
            return v
        }
        statAsr = addStat("ASR")
        statPaste = addStat("PASTE")
        statTotal = addStat("TOTAL")
        card.addView(cells)
        return card
    }

    // ================= RX / TX 状态 =================

    private fun applyRxState() {
        transmitting = false
        chipLed.background = circle(C_RX, 8f)
        chipLabel.text = "RX"
        chipLabel.setTextColor(C_RX)
        ptt.background = getDrawable(R.drawable.ptt_idle)
        pttMic.setColorFilter(C_ACCENT)
        pttLabel.text = "按住说话"
        pttLabel.setTextColor(0xFFC8CAD4.toInt())
        pttSub.text = "PUSH TO TALK"
        pttSub.setTextColor(0x4DFFFFFF.toInt())
        grille.lineColor = 0x1AFFFFFF.toInt()
        signalBars.live = false
        waveform.visibility = View.GONE
    }

    private fun applyTxState() {
        transmitting = true
        chipLed.background = circle(C_TX, 8f)
        chipLabel.text = "TX"
        chipLabel.setTextColor(C_TX)
        ptt.background = getDrawable(R.drawable.ptt_tx)
        pttMic.setColorFilter(C_TX)
        pttLabel.text = "松手发送"
        pttLabel.setTextColor(0xFFFFB3B5.toInt())
        pttSub.text = "TRANSMITTING"
        pttSub.setTextColor(0x8CFF5A5F.toInt())
        grille.lineColor = 0x59FF5A5F.toInt()
        signalBars.live = true
        waveform.visibility = View.VISIBLE
    }

    private fun updateMacText() {
        val s = activeServer()
        macText.text = if (s == null || s.ip.isEmpty()) "MAC · 未配置"
        else "MAC · ${s.ip}"
    }

    private fun updateMsgIdle() {
        msgText.text = "待机中 · 按住中央 PTT 说话"
    }

    // ================= 功能键 =================

    private fun fireButton(label: String, payload: String) {
        val c = client ?: return showMsg("未配置服务器，点右上角状态章添加")
        showMsg("[$label] 发送中…")
        if (payload.isEmpty()) {
            c.clear(object : ServerClient.Callback {
                override fun onOk(body: JSONObject) = showMsg("[$label] 已清空输入框")
                override fun onErr(msg: String) = showMsg("[$label] 失败：$msg")
            })
        } else {
            c.sendText(payload, object : ServerClient.Callback {
                override fun onOk(body: JSONObject) = showMsg("[$label] 已发送：$payload")
                override fun onErr(msg: String) = showMsg("[$label] 失败：$msg")
            })
        }
    }

    private fun editButton(currentLabel: String, currentPayload: String) {
        val buttons = loadButtons()
        var index = -1
        for (i in 0 until buttons.length()) {
            val o = buttons.getJSONObject(i)
            if (o.optString("label") == currentLabel &&
                o.optString("payload") == currentPayload
            ) { index = i; break }
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
            .setTitle("编辑频道 ${index + 1}")
            .setView(ctx)
            .setPositiveButton("保存") { _, _ ->
                saveButton(index, JSONObject()
                    .put("label", labelEdit.text.toString().ifEmpty { "＋" })
                    .put("payload", payloadEdit.text.toString()))
                buildUi()
            }
            .setNegativeButton("取消", null)
            .setNeutralButton("清空此键") { _, _ ->
                saveButton(index, JSONObject().put("label", "＋").put("payload", ""))
                buildUi()
            }
            .show()
    }

    // ================= 录音与转写 =================

    private fun startRecording() {
        if (!recorder.start()) {
            showMsg("录音启动失败：请检查麦克风权限")
            return
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyTxState()
        showMsg("正在发射 · 松手后转写上屏")
    }

    private fun cancelRecording() {
        recorder.cancel()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyRxState()
        updateMsgIdle()
        showMsg("已取消")
    }

    private fun stopAndSend() {
        val wav = recorder.stop()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyRxState()
        if (wav.isEmpty()) {
            showMsg("没有录到声音")
            return
        }
        showMsg("识别中…（约 ${wav.size / 32000} 秒音频）")
        statAsr.text = "…"; statPaste.text = "…"; statTotal.text = "…"
        val c = client ?: return showMsg("未配置服务器，点右上角状态章添加")
        c.transcribe(wav, object : ServerClient.Callback {
            override fun onOk(body: JSONObject) = runOnUiThread {
                val text = body.optString("text", "")
                val asr = body.optLong("asr_ms", -1)
                val paste = body.optLong("paste_ms", -1)
                val total = body.optLong("elapsed_ms", -1)
                statAsr.text = "${asr}ms"
                statPaste.text = "${paste}ms"
                statTotal.text = "${total}ms"
                showMsg(text)
            }
            override fun onErr(msg: String) = runOnUiThread {
                showMsg("失败：$msg")
            }
        })
    }

    // ================= 连接 =================

    private fun rebuildClient() {
        val s = activeServer()
        client = if (s == null || s.ip.isEmpty()) null
        else ServerClient(s.ip, s.port, s.token)
        updateMacText()
    }

    private fun ping() {
        val c = client ?: run {
            connected = false
            return
        }
        c.ping(object : ServerClient.Callback {
            override fun onOk(body: JSONObject) { connected = true }
            override fun onErr(msg: String) { connected = false }
        })
    }

    // ================= 服务器管理（点 RX/TX 章切换，长按编辑） =================

    private fun showServerDialog() {
        val ctx = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        val group = android.widget.RadioGroup(this)
        for ((i, s) in servers.withIndex()) {
            group.addView(android.widget.RadioButton(this).apply {
                text = "${s.name}  —  ${s.ip}:${s.port}"
                id = View.generateViewId()
                isChecked = (i == activeIdx)
            })
        }
        if (servers.isEmpty()) {
            ctx.addView(TextView(this).apply {
                text = "还没有电脑档案，点下方「新增」添加。"
                setTextColor(C_INK_DIM)
                setPadding(0, dp(8), 0, dp(8))
            })
        }
        ctx.addView(group)

        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
            setPadding(0, dp(16), 0, dp(4))
        }
        fun actionChip(label: String, onClick: () -> Unit): TextView =
            TextView(this).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setTextColor(C_INK)
                background = getDrawable(R.drawable.chip_pill)
                setPadding(dp(16), dp(10), dp(16), dp(10))
                setOnClickListener { onClick() }
            }
        val checkedIndex: () -> Int = {
            val v = group.findViewById<View>(group.checkedRadioButtonId)
            val idx = if (v != null) group.indexOfChild(v) else -1
            if (idx in servers.indices) idx else activeIdx
        }
        actions.addView(actionChip("新增") {
            showEditServerDialog(null) { s ->
                servers.add(s)
                if (servers.size == 1) activeIdx = 0
                persistServers(); rebuildClient(); ping()
                showServerDialog()
            }
        })
        actions.addView(actionChip("编辑") {
            val i = checkedIndex()
            if (i in servers.indices) {
                showEditServerDialog(servers[i]) { s ->
                    servers[i] = s
                    persistServers(); rebuildClient(); ping()
                    showServerDialog()
                }
            }
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { marginStart = dp(8) })
        actions.addView(actionChip("删除") {
            val i = checkedIndex()
            if (i in servers.indices) {
                servers.removeAt(i)
                if (activeIdx >= servers.size) activeIdx = (servers.size - 1).coerceAtLeast(0)
                persistServers(); rebuildClient(); ping()
                showServerDialog()
            }
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { marginStart = dp(8) })
        ctx.addView(actions)

        AlertDialog.Builder(this)
            .setTitle("切换电脑")
            .setView(ctx)
            .setPositiveButton("切换") { _, _ ->
                val v = group.findViewById<View>(group.checkedRadioButtonId)
                val idx = if (v != null) group.indexOfChild(v) else -1
                if (idx in servers.indices) activeIdx = idx
                persistServers(); rebuildClient(); ping()
                showMsg("已切换到「${activeServer()?.name ?: "无"}」")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 设置：编辑当前电脑档案 */
    private fun showSettingsDialog() {
        val cur = activeServer()
        val ctx = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        val nameEdit = EditText(this).apply {
            hint = "当前电脑名称"
            setText(cur?.name ?: "")
        }
        val ipEdit = EditText(this).apply {
            hint = "电脑局域网 IP（如 192.168.1.10）"
            setText(cur?.ip ?: "")
        }
        val portEdit = EditText(this).apply {
            hint = "端口"
            inputType = InputType.TYPE_CLASS_NUMBER
            setText((cur?.port ?: 30686).toString())
        }
        val tokenEdit = EditText(this).apply {
            hint = "token（与电脑端 config.json 一致）"
            setText(cur?.token ?: "")
        }
        ctx.addView(nameEdit); ctx.addView(ipEdit); ctx.addView(portEdit); ctx.addView(tokenEdit)
        AlertDialog.Builder(this)
            .setTitle("设置 · 当前电脑")
            .setView(ctx)
            .setPositiveButton("保存") { _, _ ->
                val updated = ServerProfile(
                    name = nameEdit.text.toString().trim().ifEmpty { cur?.name ?: "我的电脑" },
                    ip = ipEdit.text.toString().trim(),
                    port = portEdit.text.toString().toIntOrNull() ?: 30686,
                    token = tokenEdit.text.toString().trim()
                )
                if (cur != null && activeIdx in servers.indices) servers[activeIdx] = updated
                else { servers.add(updated); activeIdx = servers.size - 1 }
                persistServers(); rebuildClient(); ping()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showEditServerDialog(
        existing: ServerProfile?, onFinish: (ServerProfile) -> Unit
    ) {
        val ctx = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }
        val nameEdit = EditText(this).apply {
            hint = "名称（如：办公室 Mac、家里 PC）"
            setText(existing?.name ?: "")
        }
        val ipEdit = EditText(this).apply {
            hint = "电脑局域网 IP（如 192.168.1.10）"
            setText(existing?.ip ?: "")
        }
        val portEdit = EditText(this).apply {
            hint = "端口"
            inputType = InputType.TYPE_CLASS_NUMBER
            setText((existing?.port ?: 30686).toString())
        }
        val tokenEdit = EditText(this).apply {
            hint = "token（与电脑端 config.json 一致）"
            setText(existing?.token ?: "")
        }
        ctx.addView(nameEdit); ctx.addView(ipEdit); ctx.addView(portEdit); ctx.addView(tokenEdit)
        AlertDialog.Builder(this)
            .setTitle(if (existing == null) "新增电脑" else "编辑电脑")
            .setView(ctx)
            .setPositiveButton("保存") { _, _ ->
                onFinish(
                    ServerProfile(
                        name = nameEdit.text.toString().trim()
                            .ifEmpty { "电脑${servers.size + 1}" },
                        ip = ipEdit.text.toString().trim(),
                        port = portEdit.text.toString().toIntOrNull() ?: 30686,
                        token = tokenEdit.text.toString().trim()
                    )
                )
            }
            .setNegativeButton("取消", null)
            .show()
    }

    // ================= 按键持久化 =================

    private fun loadButtons(): JSONArray = try {
        JSONArray(prefs.getString("buttons", defaultButtons().toString())!!)
    } catch (e: Exception) {
        defaultButtons()
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

    private fun showMsg(msg: String) = runOnUiThread { msgText.text = msg }

    private fun circle(color: Int, sizeDp: Float): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(color)
            setSize(dp(sizeDp), dp(sizeDp))
        }

    private fun ensureMicPermission() {
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), 100)
        }
    }
}
