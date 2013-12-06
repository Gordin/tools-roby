require 'rubigen'
require 'rubigen/scripts/generate'

Roby.app.auto_load_models = false
Roby.app.setup
Roby.app.register_generators

gen_name = ARGV.shift
if !gen_name
    usage = "\nInstalled Generators\n"
    RubiGen::Base.sources.inject([]) do |mem, source|
        # Using an association list instead of a hash to preserve order,
        # for aesthetic reasons more than anything else.
        label = source.label.to_s.capitalize
        pair = mem.assoc(label)
        mem << (pair = [label, []]) if pair.nil?
        pair[1] |= source.names(:visible)
        mem
    end.each do |label, names|
        usage << "  #{label}: #{names.join(', ')}\n" unless names.empty?
    end
    puts usage
else
    RubiGen::Scripts::Generate.new.run(ARGV, :generator => gen_name)
end

