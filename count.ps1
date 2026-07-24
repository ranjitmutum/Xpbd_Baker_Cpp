dir .\src -Recurse *.cpp | Get-Content | Measure-Object
dir .\tests -Recurse *.cpp | Get-Content | Measure-Object
dir .\include -Recurse *.hpp | Get-Content | Measure-Object