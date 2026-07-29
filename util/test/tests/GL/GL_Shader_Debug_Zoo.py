import noobdawn as rd
from typing import List
import rdtest


class GL_Shader_Debug_Zoo(rdtest.TestCase):
    demos_test_name = 'GL_Shader_Debug_Zoo'

    def check_capture(self):
        if not self.controller.GetAPIProperties().shaderDebugging:
            rdtest.log.success("Shader debugging not enabled, skipping test")
            return

        failed = False

        x = -3
        for child in ["GLSL tests", "SPIRV tests"]:
            rdtest.log.begin_section(child)
            section = self.find_action(child)
            for test in range(len(section.children)):
                action = section.children[test]

                if not (action.flags & rd.ActionFlags.Drawcall):
                    continue

                x += 4

                self.controller.SetFrameEvent(action.eventId, False)
                pipe: rd.PipeState = self.controller.GetPipelineState()

                if not pipe.GetShaderReflection(rd.ShaderStage.Vertex).debugInfo.debuggable:
                    rdtest.log.print("Skipping undebuggable shader at {} in {}.".format(test, child))
                    return

                if not pipe.GetShaderReflection(rd.ShaderStage.Pixel).debugInfo.debuggable:
                    rdtest.log.print("Skipping undebuggable shader at {} in {}.".format(test, child))
                    return

                y = 1

                inputs = rd.DebugPixelInputs()
                inputs.primitive = 0
                inputs.sample = 0

                if action.numIndices > 3:
                    rdtest.log.print("prim 1")
                    inputs.primitive = 1

                # Debug the shader
                trace: rd.ShaderDebugTrace = self.controller.DebugPixel(x, y, inputs)

                rdtest.log.print(f"debugging {x},{y}")

                if trace.debugger is None:
                    failed = True
                    rdtest.log.error("Test {} in sub-section {} did not debug pixel".format(test, child))
                    self.controller.FreeTrace(trace)
                    continue

                _, variables = self.process_trace(trace)

                output: rd.SourceVariableMapping = self.find_output_source_var(trace, rd.ShaderBuiltin.ColorOutput, 0)

                debugged = self.evaluate_source_var(output, variables)

                try:
                    self.check_pixel_value(pipe.GetOutputTargets()[0].resource, x, y, debugged.value.f32v[0:4])
                except rdtest.TestFailureException as ex:
                    failed = True
                    rdtest.log.error("Test {} in sub-section {} did not match pixel. {}".format(test, child, str(ex)))
                    continue
                finally:
                    self.controller.FreeTrace(trace)

                rdtest.log.success("Test {} pixel in sub-section {} matched as expected".format(test, child))
                
                vtx = 1
                inst = 0
                idx = vtx

                if action.flags & rd.ActionFlags.Instanced:
                    inst = 1

                if action.flags & rd.ActionFlags.Indexed:
                    ib = pipe.GetIBuffer()

                    mesh = rd.MeshFormat()
                    mesh.indexResourceId = ib.resourceId
                    mesh.indexByteStride = ib.byteStride
                    mesh.indexByteOffset = ib.byteOffset + action.indexOffset * ib.byteStride
                    mesh.indexByteSize = ib.byteSize
                    mesh.baseVertex = action.baseVertex

                    indices = rdtest.fetch_indices(self.controller, action, mesh, 0, vtx, 1)

                    idx = indices[1]

                postvs = self.get_postvs(action, rd.MeshDataStage.VSOut, first_index=vtx, num_indices=1, instance=inst)

                trace: rd.ShaderDebugTrace = self.controller.DebugVertex(vtx, inst, idx, 0)

                if trace.debugger is None:
                    failed = True
                    rdtest.log.error("Test {} in sub-section {} did not debug vertex".format(test, child))
                    self.controller.FreeTrace(trace)
                    continue

                _, variables = self.process_trace(trace)

                outputs = 0

                for var in trace.sourceVars:
                    var: rd.SourceVariableMapping
                    if var.variables[0].type == rd.DebugVariableType.Variable and var.signatureIndex >= 0:
                        name = var.name

                        if name not in postvs[0].keys():
                            name = name.replace(".", "Block.")
                            if name not in postvs[0].keys():
                                failed = True
                                rdtest.log.error("Don't have expected output for {}".format(name))
                                continue

                        expect = postvs[0][name]
                        value = self.evaluate_source_var(var, variables)

                        if len(expect) != value.columns:
                            failed = True
                            rdtest.log.error(
                                "Output {} at EID {} has different size ({} values) to expectation ({} values)"
                                    .format(name, action.eventId, value.columns, len(expect)))
                            continue

                        compType = rd.VarTypeCompType(value.type)
                        if compType == rd.CompType.UInt:
                            debugged = list(value.value.u32v[0:value.columns])
                        elif compType == rd.CompType.SInt:
                            debugged = list(value.value.s32v[0:value.columns])
                        else:
                            debugged = list(value.value.f32v[0:value.columns])

                        if not rdtest.value_compare(expect, debugged):
                            failed = True
                            rdtest.log.error("Test {} in sub-section {} did not match vertex.\nExpected {} but got {}".format(test, child, expect, debugged))
                            break

                        is_eq, diff_amt = rdtest.value_compare_diff(expect, debugged, eps=5.0E-06)
                        if not is_eq:
                            failed = True
                            rdtest.log.error(
                                "Debugged value {} at EID {} vert {} (idx {}) instance {}: {} difference. {} doesn't exactly match postvs output {}".format(
                                    name, action.eventId, vtx, idx, inst, diff_amt, debugged, expect))

                        outputs = outputs + 1

                self.controller.FreeTrace(trace)

                rdtest.log.success("Test {} vertex in sub-section {} matched as expected".format(test, child))

            rdtest.log.end_section(child)

        if failed:
            raise rdtest.TestFailureException("Some tests were not as expected")

        rdtest.log.success("All tests matched")
