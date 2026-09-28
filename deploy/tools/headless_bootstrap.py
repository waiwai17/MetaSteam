# -*- coding: utf-8 -*-
"""无头启动引导文件（由 launcher 通过 -ExecCmds="py <本文件>" 执行）。

只做一件事：调引擎包内的驱动模块。逻辑全部在 MetaHumanSolverEngine.headless_drive
（可单测、可复用），本文件保持极薄——因为 ExecCmds 只能执行文件，不能执行包内模块。

停止：在 Saved/Config/MetaHumanSolver/ 下放 stop.flag → 当前段完成后驱动退出。
刷新身份索引：放 refresh_assets.flag → 下一轮重写 identity_assets.json。
"""

import MetaHumanSolverEngine.headless_drive as headless_drive

headless_drive.main()
