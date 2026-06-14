# ddll64.dll 鼠标控制教程

`ddll64.dll` 是一个 Windows 鼠标控制库，支持绝对移动和相对移动。适合用在外挂/自瞄等需要程序控制鼠标的场景。

---

## 一、DLL 接口

| 函数 | 签名 | 说明 |
|------|------|------|
| `OpenDevice` | `int OpenDevice()` | 打开鼠标设备，返回 1 成功 / 0 失败 |
| `MoveTo` | `void MoveTo(unsigned short x, unsigned short y)` | 绝对移动，坐标范围 0-65535（映射到整个屏幕） |
| `MoveR` | `void MoveR(int dx, int dy)` | **相对移动**，像素偏移量，正值=右/下，负值=左/上 |

**自瞄用 `MoveR`**，不要用 `MoveTo`。相对移动更平滑，不会导致视角跳变。

---

## 二、加载 DLL（C++ 代码）

把 `ddll64.dll` 放在你的 exe 同目录，或者在代码里指定绝对路径。

```cpp
#include <windows.h>

class MouseController {
public:
    bool Load(const char *dllPath) {
        m_dll = LoadLibraryA(dllPath);
        if (!m_dll) return false;

        using OpenDeviceFn = int (*)();
        auto openDev = (OpenDeviceFn)GetProcAddress(m_dll, "OpenDevice");
        if (!openDev || openDev() == 0) { FreeLibrary(m_dll); m_dll = nullptr; return false; }

        m_moveR = (MoveRFn)GetProcAddress(m_dll, "MoveR");
        if (!m_moveR) { FreeLibrary(m_dll); m_dll = nullptr; return false; }

        return true;
    }

    void MoveRelative(int dx, int dy) {
        if (m_moveR) m_moveR(dx, dy);
    }

    bool IsLoaded() const { return m_dll != nullptr; }

    ~MouseController() { if (m_dll) FreeLibrary(m_dll); }

private:
    HINSTANCE m_dll = nullptr;
    using MoveRFn = void (*)(int, int);
    MoveRFn m_moveR = nullptr;
};
```

### 使用

```cpp
MouseController mouse;
if (mouse.Load("ddll64.dll")) {
    mouse.MoveRelative(100, 0);   // 鼠标向右移动 100 像素
    mouse.MoveRelative(0, -50);   // 鼠标向上移动 50 像素
}
```

---

## 三、自瞄中的移动量计算

### 已知条件

- 目标在画面上的位置：`(targetX, targetY)`（像素坐标）
- 画面中心：`(screenCenterX, screenCenterY)` = `(screenW / 2, screenH / 2)`
- 你希望鼠标每帧朝目标靠近一段距离

### 计算每帧移动量

```cpp
float dx = targetX - screenCenterX;  // 正=目标在右侧
float dy = targetY - screenCenterY;  // 正=目标在下方

// 方案 A：平滑逼近（推荐）
// 每帧只移动距离的 N%，逐渐逼近，不会过冲
float smoothFactor = 0.15f;          // 15% per frame
int moveX = (int)(dx * smoothFactor);
int moveY = (int)(dy * smoothFactor);

// 方案 B：固定步长
int moveX = std::clamp((int)dx, -20, 20);  // 每帧最多移 20px
int moveY = std::clamp((int)dy, -20, -20);

mouse.MoveRelative(moveX, moveY);
```

### 方案 A vs 方案 B

| | 方案 A（平滑逼近） | 方案 B（固定步长） |
|--|--|--|
| 远距离 | 移动快（大步） | 移动慢（固定步长） |
| 近距离 | 移动慢（微调） | 仍然走固定步长，容易过冲 |
| 手感 | 平滑 | 生硬 |
| **推荐** | ✅ | ❌ |

---

## 四、完整的最小 demo

如果你只想快速验证 DLL 能不能用：

```cpp
#include <windows.h>
#include <cstdio>

int main() {
    HINSTANCE dll = LoadLibraryA("ddll64.dll");
    if (!dll) { printf("DLL not found\n"); return 1; }

    auto openDev = (int (*)())GetProcAddress(dll, "OpenDevice");
    if (!openDev || !openDev()) { printf("Device failed\n"); return 1; }

    auto moveR = (void (*)(int,int))GetProcAddress(dll, "MoveR");
    if (!moveR) { printf("MoveR not found\n"); return 1; }

    printf("Moving right 100px in 1 second...\n");
    Sleep(1000);
    moveR(100, 0);
    printf("Done.\n");

    FreeLibrary(dll);
    return 0;
}
```

编译：
```bash
cl test.cpp /EHsc /link user32.lib
test.exe
```

---

## 五、在游戏中实际使用时的注意事项

### 1. 不要每帧移动完整距离

如果你一帧把鼠标从画面中心移到目标位置，下一帧目标可能已经不在原来位置了，鼠标就会来回抖。

正确的做法是**每帧只移动一小段**（方案 A 或 B），让鼠标在几帧内平滑到达。

### 2. 游戏内灵敏度

不同游戏的鼠标灵敏度不一样。你可能需要加一个**灵敏度系数**来适配：

```cpp
float gameSensitivity = 0.5f;   // 游戏灵敏度补偿
int moveX = (int)(dx * smoothFactor * gameSensitivity);
int moveY = (int)(dy * smoothFactor * gameSensitivity);
```

建议做成可配置参数，在游戏里反复调整直到手感合适。

### 3. 瞄准范围

建议只在目标离画面中心较近时才触发瞄准，避免远处目标导致鼠标乱飞：

```cpp
float dist = sqrtf(dx * dx + dy * dy);
float aimRange = 500.0f;  // 500px 范围内才触发
if (dist <= aimRange) {
    mouse.MoveRelative(moveX, moveY);
}
```

### 4. 帧率与平滑

120fps 下每帧 15% = 约 7 帧到达目标。60fps 下同样 15% = 约 14 帧到达。

如果你的帧率波动大，建议用**固定步长**方案（方案 B），效果更稳定。

### 5. 管理员权限

某些游戏可能以管理员权限运行。如果 `MoveR` 没反应，尝试以管理员权限运行你自己的程序。

---

## 六、多显示器 / 高 DPI

- **多显示器**：`MoveR` 移动的是系统光标，屏幕坐标跨显示器有效。确保你的目标坐标是相对当前游戏画面计算的。
- **4K 高 DPI**：`MoveR` 使用物理像素。如果你从 3840×2160 的画面计算偏移，直接传给 `MoveR` 就行，不需要做 DPI 转换。
- **不同分辨率**：从 OBS/截屏得到的像素坐标可能和实际游戏分辨率不一致。确保坐标计算使用的是**游戏实际渲染分辨率**。

---

## 七、常见问题

### Q: `MoveR` 移动方向和预期相反？

减小 `dy` 的符号。不同游戏的 Y 轴方向可能反了（屏幕坐标系 vs 游戏世界坐标系）。调整一下就好。

### Q: DLL 加载失败（`LoadLibrary` 返回 NULL）？

把 `ddll64.dll` 放在 exe 同目录，或指定绝对路径。64 位程序必须用 64 位 DLL。

### Q: 怎么确定灵敏度？

打开游戏进训练场，把灵敏度和平滑因子设成可调参数，一边打一边调，直到感觉跟手。没有一个万能数值。
