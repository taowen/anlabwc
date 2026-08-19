android_dep = dependency('android', required: false)
if not android_dep.found()
	android_dep = cc.find_library('android', required: true)
endif
wlr_deps += android_dep

wlr_files += files(
	'backend.c',
	'output.c',
)
