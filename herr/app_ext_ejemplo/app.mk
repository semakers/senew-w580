# Ejemplo de app EXTERNA (docs/sistema.md §11): el molde que tiene que copiar una de verdad.
# Un app.mk solo puede AÑADIR a estas variables (APPEXT_SRC/INC/DEF/OPT/LIB), y sus rutas son relativas a sistema/.
# APPEXT_PC := 1 dice que esta app es portable (solo usa la API): «make pc APPS_EXT=herr/app_ext_ejemplo» la compila.
# El primero y el último delimitan los estáticos de la app (APP_ESTATICOS_INI/FIN): el orden IMPORTA.
APPEXT_SRC += herr/app_ext_ejemplo/est_ini.c herr/app_ext_ejemplo/ejemplo_ext.c herr/app_ext_ejemplo/est_fin.c
APPEXT_INC += -Iherr/app_ext_ejemplo
APPEXT_DEF +=
APPEXT_OPT := -O2
APPEXT_LIB +=
APPEXT_PC := 1
