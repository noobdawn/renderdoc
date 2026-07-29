import noobdawn as rd
import rdtest

class VK_Resource_Usage(rdtest.TestCase):
    demos_test_name = 'VK_Resource_Usage'
    resourceUsages = {}
    eids = []

    def add_action(self, action: rd.ActionDescription):
        self.eids.append(action.eventId)
        for c in action.children:
            self.add_action(c)
        for e in action.events:
            self.eids.append(e.eventId)

    def check_resource_usage(self, res: rd.ResourceDescription, expectedUsages=[]):
        usages = self.resourceUsages[res.resourceId]
        if len(usages) != len(expectedUsages):
            for u in usages:
                rdtest.log.print(f"Resource '{res.name}' {res.resourceId} usage EID:{u.eventId} usage:{u.usage.name}")
            raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Incorrect resource usages count expected:{len(expectedUsages)} actual:{len(usages)}")
        for i, u in enumerate(usages):
            eid, usage = expectedUsages[i]
            if u.usage != usage:
                raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} EID:{u.eventId} Incorrect resource usage expected:{usage.name} actual:{u.usage.name}")
            if u.eventId != eid:
                raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} usage:{u.usage.name} Incorrect resource usage EID expected:{eid} actual:{u.eventId}")

    def check_capture(self):
        # Cache the resource usage before running any replay i.e. without calling SetFrameEvent
        resources = self.controller.GetResources()
        for res in resources:
            self.resourceUsages[res.resourceId] = self.controller.GetUsage(res.resourceId)

        drawIndirectCount = self.find_action("Draw Indirect Count") is not None
        rdtest.log.print(f"Has Draw Indirect Count: {'Yes' if drawIndirectCount else 'No'}")

        nestedSecondaries = self.find_action("Nested Secondary Command Buffer") is not None
        rdtest.log.print(f"Has Nested Secondary Command Buffer: {'Yes' if nestedSecondaries else 'No'}")

        descBuffer = self.find_action("Descriptor Buffer") is not None
        rdtest.log.print(f"Has Descriptor Buffer: {'Yes' if descBuffer else 'No'}")

        meshShader = self.find_action("Mesh Shader") is not None
        rdtest.log.print(f"Has Mesh Shader: {'Yes' if meshShader else 'No'}")

        countDrawIndirectCount = 30 if drawIndirectCount else 0
        countNested = 39 if nestedSecondaries else 0
        countDescBufferCopy = 10 if descBuffer else 0
        countDescBuffer = 21 if descBuffer else 0
        countDescBuffer += countDescBufferCopy
        countMeshShader = 33 if meshShader else 0

        action = self.find_action("Draw")
        self.controller.SetFrameEvent(action.eventId, False)
        swapImage = self.controller.GetPipelineState().GetOutputTargets()[0].resource

        with rdtest.log.auto_section("Checking Resource Usage"):
            for res in self.controller.GetResources():
                expectedUsage = []
                if res.type == rd.ResourceType.Device:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Queue:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Pool:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.SwapchainImage:
                    # the swap chain image has usage, anything else does not
                    if res.resourceId == swapImage:
                        expectedUsage = [(6,rd.ResourceUsage.Barrier), 
                                        (6,rd.ResourceUsage.Discard), 
                                        (7,rd.ResourceUsage.Clear), 
                                        (8,rd.ResourceUsage.Barrier), 
                                        (32,rd.ResourceUsage.ColorTarget), 
                                        (35,rd.ResourceUsage.ColorTarget), 
                                        (42,rd.ResourceUsage.ColorTarget), 
                                        (45,rd.ResourceUsage.ColorTarget), 
                                        (59,rd.ResourceUsage.ColorTarget), 
                                        (62,rd.ResourceUsage.ColorTarget), 
                                        (73,rd.ResourceUsage.ColorTarget), 
                                        (76,rd.ResourceUsage.ColorTarget), 
                                        (120,rd.ResourceUsage.ColorTarget), 
                                        (124,rd.ResourceUsage.ColorTarget), 
                                        (128,rd.ResourceUsage.ColorTarget), 
                                        (129,rd.ResourceUsage.ColorTarget), 
                                        (130,rd.ResourceUsage.ColorTarget), 
                                        (131,rd.ResourceUsage.ColorTarget), 
                                        (136,rd.ResourceUsage.ColorTarget), 
                                        (137,rd.ResourceUsage.ColorTarget), 
                                        (138,rd.ResourceUsage.ColorTarget), 
                                        (142,rd.ResourceUsage.ColorTarget), 
                                        (177,rd.ResourceUsage.ColorTarget), 
                                        (181,rd.ResourceUsage.ColorTarget), 
                                        (182,rd.ResourceUsage.ColorTarget), 
                                        (183,rd.ResourceUsage.ColorTarget), 
                                        (184,rd.ResourceUsage.ColorTarget), 
                                        (189,rd.ResourceUsage.ColorTarget), 
                                        (190,rd.ResourceUsage.ColorTarget), 
                                        (191,rd.ResourceUsage.ColorTarget), 
                                        (208,rd.ResourceUsage.ColorTarget), 
                                        (212,rd.ResourceUsage.ColorTarget), 
                                        (213,rd.ResourceUsage.ColorTarget), 
                                        (214,rd.ResourceUsage.ColorTarget), 
                                        (215,rd.ResourceUsage.ColorTarget), 
                                        (220,rd.ResourceUsage.ColorTarget), 
                                        (221,rd.ResourceUsage.ColorTarget), 
                                        (222,rd.ResourceUsage.ColorTarget), 
                                        (227,rd.ResourceUsage.ColorTarget)] 
                        if drawIndirectCount:
                            expectedUsage += [
                                        (232,rd.ResourceUsage.ColorTarget), 
                                        (235,rd.ResourceUsage.ColorTarget), 
                                        (239,rd.ResourceUsage.ColorTarget), 
                                        (243,rd.ResourceUsage.ColorTarget), 
                                        (248,rd.ResourceUsage.ColorTarget), 
                                        (249,rd.ResourceUsage.ColorTarget), 
                                        (250,rd.ResourceUsage.ColorTarget), 
                                        (255,rd.ResourceUsage.ColorTarget), 
                                        (256,rd.ResourceUsage.ColorTarget)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (251+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.ColorTarget), 
                                        (254+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.ColorTarget)] 
                        if descBuffer:
                            expectedUsage += [
                                        (256+countDrawIndirectCount+countNested,rd.ResourceUsage.ColorTarget), 
                                        (259+countDrawIndirectCount+countNested,rd.ResourceUsage.ColorTarget)] 
                        if meshShader:
                            expectedUsage += [
                                        (241+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (245+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (246+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (249+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (252+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (256+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (260+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget), 
                                        (264+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.ColorTarget)] 

                        expectedUsage += [(235+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier)]
                    else:
                        expectedUsage = []
                elif res.type == rd.ResourceType.RenderPass:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Sync:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.View:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Memory:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.ShaderBinding:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Shader:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.PipelineState:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Buffer:
                    if (res.name == "Vertex Buffer"):
                        expectedUsage = [(32,rd.ResourceUsage.VertexBuffer), 
                                        (35,rd.ResourceUsage.VertexBuffer),
                                        (42,rd.ResourceUsage.VertexBuffer),
                                        (45,rd.ResourceUsage.VertexBuffer),
                                        (59,rd.ResourceUsage.VertexBuffer), 
                                        (62,rd.ResourceUsage.VertexBuffer), 
                                        (73,rd.ResourceUsage.VertexBuffer), 
                                        (76,rd.ResourceUsage.VertexBuffer), 
                                        (120,rd.ResourceUsage.VertexBuffer), 
                                        (124,rd.ResourceUsage.VertexBuffer), 
                                        (128,rd.ResourceUsage.VertexBuffer), 
                                        (129,rd.ResourceUsage.VertexBuffer), 
                                        (130,rd.ResourceUsage.VertexBuffer), 
                                        (131,rd.ResourceUsage.VertexBuffer), 
                                        (136,rd.ResourceUsage.VertexBuffer), 
                                        (137,rd.ResourceUsage.VertexBuffer), 
                                        (138,rd.ResourceUsage.VertexBuffer), 
                                        (142,rd.ResourceUsage.VertexBuffer), 
                                        (177,rd.ResourceUsage.VertexBuffer), 
                                        (181,rd.ResourceUsage.VertexBuffer), 
                                        (182,rd.ResourceUsage.VertexBuffer), 
                                        (183,rd.ResourceUsage.VertexBuffer), 
                                        (184,rd.ResourceUsage.VertexBuffer), 
                                        (189,rd.ResourceUsage.VertexBuffer), 
                                        (190,rd.ResourceUsage.VertexBuffer), 
                                        (191,rd.ResourceUsage.VertexBuffer), 
                                        (208,rd.ResourceUsage.VertexBuffer), 
                                        (212,rd.ResourceUsage.VertexBuffer), 
                                        (213,rd.ResourceUsage.VertexBuffer), 
                                        (214,rd.ResourceUsage.VertexBuffer), 
                                        (215,rd.ResourceUsage.VertexBuffer), 
                                        (220,rd.ResourceUsage.VertexBuffer), 
                                        (221,rd.ResourceUsage.VertexBuffer), 
                                        (222,rd.ResourceUsage.VertexBuffer),
                                        (227,rd.ResourceUsage.VertexBuffer)] 
                        if drawIndirectCount:
                            expectedUsage += [
                                        (232,rd.ResourceUsage.VertexBuffer), 
                                        (235,rd.ResourceUsage.VertexBuffer), 
                                        (239,rd.ResourceUsage.VertexBuffer), 
                                        (243,rd.ResourceUsage.VertexBuffer), 
                                        (248,rd.ResourceUsage.VertexBuffer), 
                                        (249,rd.ResourceUsage.VertexBuffer), 
                                        (250,rd.ResourceUsage.VertexBuffer), 
                                        (255,rd.ResourceUsage.VertexBuffer), 
                                        (256,rd.ResourceUsage.VertexBuffer)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (251+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.VertexBuffer), 
                                        (254+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.VertexBuffer)]
                        if descBuffer:
                            expectedUsage += [
                                        (256+countDrawIndirectCount+countNested,rd.ResourceUsage.VertexBuffer), 
                                        (259+countDrawIndirectCount+countNested,rd.ResourceUsage.VertexBuffer)]
                    if (res.name == "Index Buffer"):
                        expectedUsage = [(35,rd.ResourceUsage.IndexBuffer),
                                        (45,rd.ResourceUsage.IndexBuffer),
                                        (62,rd.ResourceUsage.IndexBuffer),
                                        (76,rd.ResourceUsage.IndexBuffer),
                                        (136,rd.ResourceUsage.IndexBuffer),
                                        (137,rd.ResourceUsage.IndexBuffer),
                                        (138,rd.ResourceUsage.IndexBuffer),
                                        (142,rd.ResourceUsage.IndexBuffer),
                                        (189,rd.ResourceUsage.IndexBuffer),
                                        (190,rd.ResourceUsage.IndexBuffer),
                                        (191,rd.ResourceUsage.IndexBuffer),
                                        (220,rd.ResourceUsage.IndexBuffer),
                                        (221,rd.ResourceUsage.IndexBuffer),
                                        (222,rd.ResourceUsage.IndexBuffer)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (235,rd.ResourceUsage.IndexBuffer), 
                                        (243,rd.ResourceUsage.IndexBuffer), 
                                        (255,rd.ResourceUsage.IndexBuffer),
                                        (256,rd.ResourceUsage.IndexBuffer)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (254+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.IndexBuffer)]
                        if descBuffer:
                            expectedUsage += [
                                        (259+countDrawIndirectCount+countNested,rd.ResourceUsage.IndexBuffer)]
                    if (res.name == "Compute Buffer In"):
                        expectedUsage += [(89,rd.ResourceUsage.CS_Constants),
                                        (96,rd.ResourceUsage.CS_Constants)]
                        if nestedSecondaries:
                            expectedUsage += [(267+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.CS_Constants)]
                        if descBuffer:
                            expectedUsage += [(264+countDrawIndirectCount+countNested,rd.ResourceUsage.CS_Constants)]
                    if (res.name == "Compute Buffer Out"):
                        expectedUsage += [(89,rd.ResourceUsage.CS_RWResource),
                                        (96,rd.ResourceUsage.CS_RWResource)]
                        if nestedSecondaries:
                            expectedUsage += [(267+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.CS_RWResource)]
                        if descBuffer:
                            expectedUsage += [(264+countDrawIndirectCount+countNested,rd.ResourceUsage.CS_RWResource)]
                    if (res.name == "Indirect Data"):
                        expectedUsage += [(14,rd.ResourceUsage.Barrier),
                                        (15,rd.ResourceUsage.Clear),
                                        (16,rd.ResourceUsage.Barrier),
                                        (20,rd.ResourceUsage.CS_RWResource),
                                        (21,rd.ResourceUsage.Barrier),
                                        (97,rd.ResourceUsage.Barrier),
                                        (108,rd.ResourceUsage.CS_RWResource),
                                        (108,rd.ResourceUsage.Indirect),
                                        (109,rd.ResourceUsage.Barrier),
                                        (120,rd.ResourceUsage.Indirect),
                                        (124,rd.ResourceUsage.Indirect),
                                        (127,rd.ResourceUsage.Indirect),
                                        (135,rd.ResourceUsage.Indirect),
                                        (142,rd.ResourceUsage.Indirect),
                                        (144,rd.ResourceUsage.Barrier),
                                        (149,rd.ResourceUsage.Barrier),
                                        (150,rd.ResourceUsage.Clear),
                                        (151,rd.ResourceUsage.Barrier),
                                        (155,rd.ResourceUsage.CS_RWResource),
                                        (157,rd.ResourceUsage.Barrier),
                                        (159,rd.ResourceUsage.CS_RWResource),
                                        (159,rd.ResourceUsage.Indirect),
                                        (160,rd.ResourceUsage.CS_RWResource),
                                        (160,rd.ResourceUsage.Indirect),
                                        (161,rd.ResourceUsage.Barrier),
                                        (162,rd.ResourceUsage.CS_RWResource),
                                        (162,rd.ResourceUsage.Indirect),
                                        (163,rd.ResourceUsage.Barrier),
                                        (177,rd.ResourceUsage.Indirect),
                                        (180,rd.ResourceUsage.Indirect),
                                        (188,rd.ResourceUsage.Indirect),
                                        (208,rd.ResourceUsage.Indirect),
                                        (211,rd.ResourceUsage.Indirect),
                                        (219,rd.ResourceUsage.Indirect),
                                        (227,rd.ResourceUsage.IndexBuffer),
                                        (227,rd.ResourceUsage.Indirect)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (232,rd.ResourceUsage.Indirect),
                                        (232,rd.ResourceUsage.Indirect),
                                        (235,rd.ResourceUsage.Indirect),
                                        (235,rd.ResourceUsage.Indirect),
                                        (239,rd.ResourceUsage.Indirect),
                                        (239,rd.ResourceUsage.Indirect),
                                        (243,rd.ResourceUsage.Indirect),
                                        (243,rd.ResourceUsage.Indirect),
                                        (247,rd.ResourceUsage.Indirect),
                                        (247,rd.ResourceUsage.Indirect),
                                        (254,rd.ResourceUsage.Indirect),
                                        (254,rd.ResourceUsage.Indirect)]
                        expectedUsage += [(231+countDrawIndirectCount,rd.ResourceUsage.Barrier)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (268+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.Barrier)]
                        if meshShader:
                            expectedUsage += [
                                        (244+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (249+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (252+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (255+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (255+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (260+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (260+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (264+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect),
                                        (264+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Indirect)]
                    if (res.name == "Barrier Buffer"):
                        expectedUsage = [(242+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (250+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (258+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (266+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (274+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (282+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (290+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (298+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (306+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (314+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier)]
                    if (res.name == "Barrier2 Buffer"):
                        expectedUsage = [(322+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (327+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (332+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier),
                                        (337+countDrawIndirectCount+countNested+countDescBuffer+countMeshShader,rd.ResourceUsage.Barrier)]
                    if (res.name == "Descriptor Buffer"):
                        if descBuffer:
                            expectedUsage = [(235+countDrawIndirectCount,rd.ResourceUsage.Barrier), 
                                        (236+countDrawIndirectCount,rd.ResourceUsage.CopySrc),
                                        (237+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (238+countDrawIndirectCount,rd.ResourceUsage.Clear),
                                        (241+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (242+countDrawIndirectCount,rd.ResourceUsage.CopyDst)]
                    if (res.name == "Descriptor Backup Buffer"):
                        if descBuffer:
                            expectedUsage = [(235+countDrawIndirectCount,rd.ResourceUsage.Barrier), 
                                        (236+countDrawIndirectCount,rd.ResourceUsage.CopyDst),
                                        (241+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (242+countDrawIndirectCount,rd.ResourceUsage.CopySrc)]
                elif res.type == rd.ResourceType.Texture:
                    if (res.name == "Offscreen MSAA Image"):
                        expectedUsage = [(11,rd.ResourceUsage.Barrier), 
                                        (11,rd.ResourceUsage.Discard), 
                                        (12,rd.ResourceUsage.Clear)]
                    if (res.name == "Offscreen Image"):
                        expectedUsage = [(9,rd.ResourceUsage.Barrier), 
                                        (9,rd.ResourceUsage.Discard), 
                                        (10,rd.ResourceUsage.Clear), 
                                        (42,rd.ResourceUsage.PS_Resource), 
                                        (45,rd.ResourceUsage.PS_Resource), 
                                        (120,rd.ResourceUsage.PS_Resource), 
                                        (124,rd.ResourceUsage.PS_Resource), 
                                        (128,rd.ResourceUsage.PS_Resource), 
                                        (129,rd.ResourceUsage.PS_Resource), 
                                        (130,rd.ResourceUsage.PS_Resource), 
                                        (131,rd.ResourceUsage.PS_Resource), 
                                        (136,rd.ResourceUsage.PS_Resource), 
                                        (137,rd.ResourceUsage.PS_Resource), 
                                        (138,rd.ResourceUsage.PS_Resource), 
                                        (142,rd.ResourceUsage.PS_Resource), 
                                        (177,rd.ResourceUsage.PS_Resource), 
                                        (181,rd.ResourceUsage.PS_Resource), 
                                        (182,rd.ResourceUsage.PS_Resource), 
                                        (183,rd.ResourceUsage.PS_Resource), 
                                        (184,rd.ResourceUsage.PS_Resource), 
                                        (189,rd.ResourceUsage.PS_Resource), 
                                        (190,rd.ResourceUsage.PS_Resource), 
                                        (191,rd.ResourceUsage.PS_Resource), 
                                        (208,rd.ResourceUsage.PS_Resource), 
                                        (212,rd.ResourceUsage.PS_Resource), 
                                        (213,rd.ResourceUsage.PS_Resource), 
                                        (214,rd.ResourceUsage.PS_Resource), 
                                        (215,rd.ResourceUsage.PS_Resource), 
                                        (220,rd.ResourceUsage.PS_Resource), 
                                        (221,rd.ResourceUsage.PS_Resource), 
                                        (222,rd.ResourceUsage.PS_Resource),
                                        (227,rd.ResourceUsage.PS_Resource)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (232,rd.ResourceUsage.PS_Resource), 
                                        (235,rd.ResourceUsage.PS_Resource), 
                                        (239,rd.ResourceUsage.PS_Resource), 
                                        (243,rd.ResourceUsage.PS_Resource), 
                                        (248,rd.ResourceUsage.PS_Resource), 
                                        (249,rd.ResourceUsage.PS_Resource), 
                                        (250,rd.ResourceUsage.PS_Resource), 
                                        (255,rd.ResourceUsage.PS_Resource), 
                                        (256,rd.ResourceUsage.PS_Resource)]
                        if descBuffer:
                            expectedUsage += [
                                        (256+countDrawIndirectCount+countNested,rd.ResourceUsage.PS_Resource), 
                                        (259+countDrawIndirectCount+countNested,rd.ResourceUsage.PS_Resource)]
                elif res.type == rd.ResourceType.CommandBuffer:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.DescriptorStore:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Sampler:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                else:
                    raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Unexpected resource type {res.type.name}")
                rdtest.log.print(f"Resource '{res.name}' type:{res.type.name} {res.resourceId} usages:{len(self.controller.GetUsage(res.resourceId))} expectedUsages:{len(expectedUsage)}")
                self.check_resource_usage(res, expectedUsage)

        actions = self.controller.GetRootActions()
        for a in actions:
            self.add_action(a)

        # Select every event of the resource usage to ensure the EID is valid
        with rdtest.log.auto_section("Checking Resource Usage Events can be replayed"):
            for res in self.controller.GetResources():
                rdtest.log.print(f"Resource '{res.name}' type:{res.type.name} {res.resourceId}")
                usages = self.resourceUsages[res.resourceId]
                for u in usages:
                    eid = u.eventId
                    if eid == 0:
                        continue
                    self.controller.SetFrameEvent(eid, True)
                    if eid not in self.eids:
                        raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Missing EID:{eid}")
        
        with rdtest.log.auto_section("Checking Indirect Action Names"):
            if not self.check_indirect_action_name_consistency(self.controller):
                raise rdtest.TestFailureException("Indirect action parameters do not match its event parameters")