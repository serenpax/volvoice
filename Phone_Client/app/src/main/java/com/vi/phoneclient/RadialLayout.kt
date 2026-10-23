package com.vi.phoneclient

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.ValueAnimator
import android.content.Context
import android.os.SystemClock
import android.util.AttributeSet
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.view.animation.DecelerateInterpolator
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.roundToLong
import kotlin.math.sin

/**
 * 左轮式径向布局（单手操作）：
 *  - child 0 = 中央大按钮（按住说话），固定不动
 *  - 其余子视图均布在圆环上，可用拇指整体拨动旋转（左轮弹巢）
 *  - 带惯性：甩动后继续滑行，并"咔哒"吸附到最近的固定槽位（含轻震动反馈）
 *  - 只负责拨位置；触发仍靠正常点按（点按/长按编辑由子视图处理）
 */
class RadialLayout @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : ViewGroup(context, attrs) {

    private var angleOffset = 0.0        // 弧度
    private var lastAngle = 0.0
    private var downX = 0f
    private var downY = 0f
    private var rotating = false
    private var tracking = false

    // 惯性：角速度（rad/s，平滑）
    private var velocity = 0.0
    private var lastMoveTime = 0L

    private var settleAnimator: ValueAnimator? = null

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop

    // ---------------- 测量与布局 ----------------

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        val base = min(w, h).toFloat()
        val centerD = (base * 0.37f).toInt()
        val satD = (base * 0.17f).toInt()
        for (i in 0 until childCount) {
            val d = if (i == 0) centerD else satD
            getChildAt(i).measure(
                MeasureSpec.makeMeasureSpec(d, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(d, MeasureSpec.EXACTLY)
            )
        }
        setMeasuredDimension(w, h)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        if (childCount == 0) return
        val cx = (r - l) / 2
        val cy = (b - t) / 2
        val base = min(r - l, b - t).toFloat()

        val center = getChildAt(0)
        val cd = center.measuredWidth
        center.layout(cx - cd / 2, cy - cd / 2, cx + cd / 2, cy + cd / 2)

        val satellites = childCount - 1
        if (satellites == 0) return
        val sd = getChildAt(1).measuredWidth
        val radius = base * 0.35f
        for (i in 1 until childCount) {
            val angle = -PI / 2 + 2 * PI * (i - 1) / satellites + angleOffset
            val x = cx + (radius * cos(angle)).toInt()
            val y = cy + (radius * sin(angle)).toInt()
            val child = getChildAt(i)
            child.layout(x - sd / 2, y - sd / 2, x + sd / 2, y + sd / 2)
        }
    }

    // ---------------- 旋转手势 ----------------

    private fun angleOf(x: Float, y: Float): Double =
        atan2((y - height / 2f).toDouble(), (x - width / 2f).toDouble())

    private fun onCenterChild(x: Float, y: Float): Boolean {
        if (childCount == 0) return true
        val c = getChildAt(0)
        return x >= c.left && x <= c.right && y >= c.top && y <= c.bottom
    }

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = ev.x
                downY = ev.y
                lastAngle = angleOf(ev.x, ev.y)
                rotating = false
                tracking = !onCenterChild(ev.x, ev.y)
                velocity = 0.0
                lastMoveTime = SystemClock.uptimeMillis()
                settleAnimator?.cancel()  // 手指按下即接管未完成的吸附动画
            }
            MotionEvent.ACTION_MOVE -> {
                if (tracking && !rotating) {
                    val dx = ev.x - downX
                    val dy = ev.y - downY
                    if (dx * dx + dy * dy > touchSlop * touchSlop) {
                        rotating = true
                        return true
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> tracking = false
        }
        return false
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_MOVE -> if (rotating) {
                val now = SystemClock.uptimeMillis()
                val a = angleOf(ev.x, ev.y)
                var delta = a - lastAngle
                while (delta > PI) delta -= 2 * PI
                while (delta < -PI) delta += 2 * PI
                angleOffset += delta
                lastAngle = a

                // 角速度估计（平滑）
                val dt = (now - lastMoveTime) / 1000.0
                if (dt > 0.001) {
                    val inst = delta / dt
                    velocity = velocity * 0.6 + inst * 0.4
                }
                lastMoveTime = now

                requestLayout()
                invalidate()
                return true
            }
            MotionEvent.ACTION_UP -> if (rotating) {
                rotating = false
                tracking = false
                flingToSlot()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                rotating = false
                tracking = false
                settleToNearestSlot()
                return true
            }
        }
        return rotating
    }

    // ---------------- 惯性 + 咔哒吸附 ----------------

    private fun slotStep(): Double {
        val n = childCount - 1
        return if (n > 0) 2 * PI / n else 2 * PI
    }

    /** 甩动惯性：按角速度外推一小段，再吸附到最近的固定槽位 */
    private fun flingToSlot() {
        // 松手越快外推越远，封顶约 1.2 个槽位，防止"转飞"
        val step = slotStep()
        var projected = angleOffset + velocity * 0.12
        val maxExtra = step * 1.2
        val extra = projected - angleOffset
        if (extra > maxExtra) projected = angleOffset + maxExtra
        if (extra < -maxExtra) projected = angleOffset - maxExtra
        settleTo(projected)
    }

    private fun settleToNearestSlot() {
        settleTo(angleOffset)
    }

    private fun settleTo(projected: Double) {
        val step = slotStep()
        val snap = Math.round(projected / step) * step
        val travel = abs(snap - angleOffset)

        if (travel < 1e-4) {
            angleOffset = snap
            requestLayout()
            return
        }

        // 距离越远动画越久（180~380ms），减速插值模拟"咔哒"入位
        val duration = (180 + travel / step * 220).toLong().coerceAtMost(380)
        settleAnimator?.cancel()
        val anim = ValueAnimator.ofFloat(0f, 1f).setDuration(duration)
        anim.interpolator = DecelerateInterpolator(1.8f)
        val start = angleOffset
        anim.addUpdateListener { a ->
            angleOffset = start + (snap - start) * (a.animatedValue as Float)
            requestLayout()
            invalidate()
        }
        anim.addListener(object : AnimatorListenerAdapter() {
            override fun onAnimationEnd(animation: Animator) {
                angleOffset = snap
                requestLayout()
                invalidate()
                // 咔哒：轻震动反馈
                performHapticFeedback(
                    HapticFeedbackConstants.CLOCK_TICK,
                    HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING
                )
            }
        })
        anim.start()
        settleAnimator = anim
    }
}
