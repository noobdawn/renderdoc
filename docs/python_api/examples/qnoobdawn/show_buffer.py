filename = "test.nbd"
formatter = "float3 pos; half norms[16]; uint flags;"

pynoobdawn.LoadCapture(filename, noobdawn.ReplayOptions(), filename, False, True)

mybuf = noobdawn.ResourceId.Null()

for buf in pynoobdawn.GetBuffers():
    print("buf %s is %s" % (buf.resourceId, pynoobdawn.GetResourceName(buf.resourceId)))

    # here put your actual selection criteria - i.e. look for a particular name
    if pynoobdawn.GetResourceName(buf.resourceId) == "dataBuffer":
        mybuf = buf.resourceId
        break

print("selected %s" % pynoobdawn.GetResourceName(mybuf))

if mybuf != noobdawn.ResourceId.Null():
	# Open a new buffer viewer for this buffer, with the given format
	bufview = pynoobdawn.ViewBuffer(0, 0, mybuf, formatter)

	# Show the buffer viewer on the main tool area
	pynoobdawn.AddDockWindow(bufview.Widget(), qnoobdawn.DockReference.MainToolArea, None)
