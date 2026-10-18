# 第三方依赖统一在这里声明 imported target。
# Phase 1 不引入第三方库；Phase 4 补 mysqlclient/hiredis，Phase 9 补 FFmpeg/SDL2。
# 约定：业务 target 只链接 cm::mysql、cm::hiredis、cm::ffmpeg、cm::sdl2、cm::gtest，
#       不直接写 -l 或绝对路径。
