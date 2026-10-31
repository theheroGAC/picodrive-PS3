static const char *men_hscaling_opts[] = { "1:1", "4:3", "extended", "fullwidth", NULL };

#define MENU_OPTIONS_GFX \
	mee_enum    ("Aspect ratio",       MA_OPT_SCALING,    currentConfig.scaling, men_hscaling_opts), \
	mee_onoff   ("Wait for vsync",     MA_OPT3_VSYNC,     currentConfig.EmuOpt, EOPT_VSYNC), \

#define MENU_OPTIONS_ADV

static menu_entry e_menu_sms_options[];
static menu_entry e_menu_keyconfig[];

void psp_menu_init(void)
{
	me_enable(e_menu_sms_options, MA_SMSOPT_GHOSTING, 0);
	me_enable(e_menu_keyconfig, MA_CTRL_DEADZONE, 1);
}
