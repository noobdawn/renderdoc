import noobdawn as rd
import rdtest


class D3D12_Annotations(rdtest.Annotations):
    demos_test_name = 'D3D12_Annotations'
    internal = False

    def check_capture(self):
        super().check_resource_annotations()
        super().check_command_annotations(True)