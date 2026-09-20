android_dep = dependency('android', required: false)
if not android_dep.found()
	android_dep = cc.find_library('android', required: true)
endif
egl_dep = dependency('egl', required: false)
if not egl_dep.found()
	egl_dep = cc.find_library('EGL', required: true)
endif
gles_dep = dependency('glesv2', required: false)
if not gles_dep.found()
	gles_dep = cc.find_library('GLESv2', required: true)
endif
wlr_deps += [android_dep, egl_dep, gles_dep]

wlr_files += files(
	'backend.c',
	'output.c',
	'../../render/android.c',
)
