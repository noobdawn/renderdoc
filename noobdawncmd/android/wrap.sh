#!/system/bin/sh
export noobdawn__replay__marker=1
# chain to asan's wrap if needed, now that we exported the env var
if [ -f asan.sh ]; then
  ./asan.sh "$@"
else
  exec "$@"
fi
