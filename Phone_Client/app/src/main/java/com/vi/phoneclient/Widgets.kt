package com.vi.phoneclient

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * 方案 E 自定义部件（全部程序绘制，零图片资源）：
 *  - DialTicksView  频率刻度环（发丝密齿 + 45° 长齿）
 *  - GrilleView     扬声器格栅 / 机身握持纹
 *  - WaveformView   发射波形
 *  - SignalBarsView 信号格
 */

private fun Context.dpx(v: Float): Float = v * resources.displayMetrics.density

// ---------------- 刻度环 ----------------

class DialTicksView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeCap = Paint.Cap.BUTT
    }
    private val minorColor = 0x38FFFFFF.toInt()   // ~22% 白
    private val majorColor = 0x66FFFFFF.toInt()   // ~40% 白

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val base = min(width, height).toFloat()
        val rOuter = base * 0.485f
        val minorLen = context.dpx(4f)
        val majorLen = context.dpx(7f)
        paint.strokeWidth = context.dpx(0.8f)

        // 64 根密齿（每 5.625°），其中每 8 根一根长齿（45°）
        for (i in 0 until 64) {
            val isMajor = i % 8 == 0
            val len = if (isMajor) majorLen else minorLen
            paint.color = if (isMajor) majorColor else minorColor
            val angle = Math.toRadians(i * 5.625 - 90.0)
            val cosA = cos(angle).toFloat()
            val sinA = sin(angle).toFloat()
            canvas.drawLine(
                cx + cosA * (rOuter - len), cy + sinA * (rOuter - len),
                cx + cosA * rOuter, cy + sinA * rOuter,
                paint
            )
        }
    }
}

// ---------------- 格栅 / 握持纹 ----------------

class GrilleView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    var lineColor: Int = 0x1AFFFFFF.toInt()
        set(v) { field = v; invalidate() }
    var horizontal: Boolean = true
    var lineAlpha: Float = 1f

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeCap = Paint.Cap.BUTT
    }

    override fun onDraw(canvas: Canvas) {
        paint.color = lineColor
        paint.strokeWidth = context.dpx(1f)
        val gap = context.dpx(3.5f)
        if (horizontal) {
            var y = context.dpx(1f)
            while (y < height) {
                canvas.drawLine(0f, y, width.toFloat(), y, paint)
                y += gap
            }
        } else {
            var x = context.dpx(1f)
            while (x < width) {
                canvas.drawLine(x, 0f, x, height.toFloat(), paint)
                x += gap
            }
        }
    }
}

// ---------------- 发射波形 ----------------

class WaveformView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    // 18 根柱子的相对高度（0..1），与设计稿一致
    private val ratios = floatArrayOf(
        .27f, .55f, .82f, .45f, .91f, .64f, .36f, .73f, 1f,
        .55f, .32f, .68f, .86f, .41f, .59f, .77f, .36f, .23f
    )
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFF5A5F.toInt()
        setShadowLayer(context.dpx(3f), 0f, 0f, 0x80FF5A5F.toInt())
    }

    init {
        setLayerType(LAYER_TYPE_SOFTWARE, null)
    }

    override fun onDraw(canvas: Canvas) {
        val barW = context.dpx(3f)
        val gap = context.dpx(3f)
        val maxH = context.dpx(22f)
        val cy = height / 2f
        var x = 0f
        for (r in ratios) {
            val h = maxH * r
            canvas.drawRoundRect(
                x, cy - h / 2, x + barW, cy + h / 2,
                barW / 2, barW / 2, paint
            )
            x += barW + gap
        }
    }
}

// ---------------- 信号格 ----------------

class SignalBarsView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    /** 发射态：全红发光；待机：靛蓝，第 5 格熄灭 */
    var live: Boolean = false
        set(v) { field = v; invalidate() }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun onDraw(canvas: Canvas) {
        val barW = context.dpx(3.5f)
        val gap = context.dpx(2.5f)
        val heights = floatArrayOf(4f, 6f, 8f, 10f, 12f)
        var x = 0f
        val bottom = height.toFloat()
        for (i in heights.indices) {
            val h = context.dpx(heights[i])
            val off = !live && i == 4
            paint.color = when {
                off -> 0x1AFFFFFF.toInt()
                live -> 0xFFFF5A5F.toInt()
                else -> 0xFF7C8CFF.toInt()
            }
            canvas.drawRoundRect(
                x, bottom - h, x + barW, bottom,
                context.dpx(1f), context.dpx(1f), paint
            )
            x += barW + gap
        }
    }
}
